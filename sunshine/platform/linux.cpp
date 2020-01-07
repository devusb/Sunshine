//
// Created by loki on 6/21/19.
//

#include "common.h"
#include "../main.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <xcb/shm.h>
#include <xcb/xfixes.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <pulse/simple.h>
#include <pulse/error.h>

#include <iostream>
#include <bitset>
#include <sunshine/task_pool.h>

namespace platf {
using namespace std::literals;

void freeImage(XImage *);

using ifaddr_t = util::safe_ptr<ifaddrs, freeifaddrs>;
using xcb_connect_t = util::safe_ptr<xcb_connection_t, xcb_disconnect>;
using xcb_img_t = util::c_ptr<xcb_shm_get_image_reply_t>;
using xcb_cursor_img = util::c_ptr<xcb_xfixes_get_cursor_image_reply_t>;

using xdisplay_t = util::safe_ptr_v2<Display, int, XCloseDisplay>;
using ximg_t = util::safe_ptr<XImage, freeImage>;

class shm_id_t {
public:
  shm_id_t() : id { -1 } {}
  shm_id_t(int id) : id {id } {}
  shm_id_t(shm_id_t &&other) noexcept : id(other.id) {
    other.id = -1;
  }

  ~shm_id_t() {
    if(id != -1) {
      shmctl(id, IPC_RMID, nullptr);
      id = -1;
    }
  }
  int id;
};

class shm_data_t {
public:
  shm_data_t() : data {(void*)-1 } {}
  shm_data_t(void *data) : data {data } {}

  shm_data_t(shm_data_t &&other) noexcept : data(other.data) {
    other.data = (void*)-1;
  }

  ~shm_data_t() {
    if((std::uintptr_t)data != -1) {
      shmdt(data);
      data = (void*)-1;
    }
  }

  void *data;
};

struct x11_img_t : public img_t {
  x11_img_t(std::uint8_t *data, std::int32_t width, std::int32_t height, XImage *img) : img { img } {
    this->data = data;
    this->width = width;
    this->height = height;
  }

  ximg_t img;
};

struct shm_img_t : public img_t {
  ~shm_img_t() override {
    if(data) {
      delete(data);
    }
  }
};

void blend_cursor(Display *display, std::uint8_t *img_data, int width, int height) {
  XFixesCursorImage *overlay = XFixesGetCursorImage(display);
  overlay->x -= overlay->xhot;
  overlay->y -= overlay->yhot;

  overlay->x = std::max((short)0, overlay->x);
  overlay->y = std::max((short)0, overlay->y);

  auto pixels = (int*)img_data;

  auto screen_height = height;
  auto screen_width  = width;

  auto delta_height = std::min<uint16_t>(overlay->height, std::max(0, screen_height - overlay->y));
  auto delta_width = std::min<uint16_t>(overlay->width, std::max(0, screen_width - overlay->x));
  for(auto y = 0; y < delta_height; ++y) {

    auto overlay_begin = &overlay->pixels[y * overlay->width];
    auto overlay_end   = &overlay->pixels[y * overlay->width + delta_width];

    auto pixels_begin = &pixels[(y + overlay->y) * screen_width + overlay->x];
    std::for_each(overlay_begin, overlay_end, [&](long pixel) {
      int *pixel_p = (int*)&pixel;

      auto colors_in = (uint8_t*)pixels_begin;

      auto alpha = (*(uint*)pixel_p) >> 24u;
      if(alpha == 255) {
        *pixels_begin = *pixel_p;
      }
      else {
        auto colors_out = (uint8_t*)pixel_p;
        colors_in[0] = colors_out[0] + (colors_in[0] * (255 - alpha) + 255/2) / 255;
        colors_in[1] = colors_out[1] + (colors_in[1] * (255 - alpha) + 255/2) / 255;
        colors_in[2] = colors_out[2] + (colors_in[2] * (255 - alpha) + 255/2) / 255;
      }
      ++pixels_begin;
    });
  }
}
struct x11_attr_t : public display_t {
  x11_attr_t() : xdisplay {XOpenDisplay(nullptr) }, xwindow {DefaultRootWindow(xdisplay.get()) }, xattr {} {
    refresh();
  }

  void refresh() {
    XGetWindowAttributes(xdisplay.get(), xwindow, &xattr);
  }

  std::unique_ptr<img_t> snapshot(bool cursor) override {
    refresh();
    XImage *img { XGetImage(
      xdisplay.get(),
      xwindow,
      0, 0,
      xattr.width, xattr.height,
      AllPlanes, ZPixmap)
    };

    if(!cursor) {
      return std::make_unique<x11_img_t>((std::uint8_t*)img->data, img->width, img->height, img);
    }

    blend_cursor(xdisplay.get(), (std::uint8_t*)img->data, img->width, img->height);
    return std::make_unique<x11_img_t>((std::uint8_t*)img->data, img->width, img->height, img);
  }

  xdisplay_t xdisplay;
  Window xwindow;
  XWindowAttributes xattr;
};

struct shm_attr_t : public x11_attr_t {
  xdisplay_t shm_xdisplay; // Prevent race condition with x11_attr_t::xdisplay
  xcb_connect_t xcb;
  xcb_screen_t *display;
  std::uint32_t seg;

  shm_id_t shm_id;

  shm_data_t data;

  util::TaskPool::task_id_t refresh_task_id;
  void delayed_refresh() {
    refresh();

    refresh_task_id = task_pool.pushDelayed(&shm_attr_t::delayed_refresh, 1s, this).task_id;
  }

  shm_attr_t() : x11_attr_t(), shm_xdisplay {XOpenDisplay(nullptr) } {
    refresh_task_id = task_pool.pushDelayed(&shm_attr_t::delayed_refresh, 1s, this).task_id;
  }

  ~shm_attr_t() {
    while(!task_pool.cancel(refresh_task_id));
  }

  std::unique_ptr<img_t> snapshot(bool cursor) override {
    if(display->width_in_pixels != xattr.width || display->height_in_pixels != xattr.height) {
      deinit();
      if(init()) {
        std::cout << "FATAL ERROR: Couldn't reinitialize shm_attr_t"sv << std::endl;

        std::abort();
      }
    }

    auto img_cookie = xcb_shm_get_image_unchecked(
      xcb.get(),
      display->root,
      0, 0,
      display->width_in_pixels, display->height_in_pixels,
      ~0,
      XCB_IMAGE_FORMAT_Z_PIXMAP,
      seg,
      0
    );

    xcb_img_t img_reply { xcb_shm_get_image_reply(xcb.get(), img_cookie, nullptr) };
    if(!img_reply) {
      std::cout << "Info: Could not get image reply"sv << std::endl;
      return nullptr;
    }

    auto img = std::make_unique<shm_img_t>();
    img->data = new std::uint8_t[frame_size()];
    img->width = display->width_in_pixels;
    img->height = display->height_in_pixels;

    std::copy((std::uint8_t*)data.data, (std::uint8_t*)data.data + frame_size(), img->data);

    if(!cursor) {
      return img;
    }

    blend_cursor(shm_xdisplay.get(), img->data, img->width, img->height);

    return img;
  }

  void deinit() {
    data.~shm_data_t();
    shm_id.~shm_id_t();
    xcb.reset(nullptr);
  }

  int init() {
    shm_xdisplay.reset(XOpenDisplay(nullptr));
    xcb.reset(xcb_connect(nullptr, nullptr));
    if(xcb_connection_has_error(xcb.get())) {
      return -1;
    }

    if(!xcb_get_extension_data(xcb.get(), &xcb_shm_id)->present) {
      std::cout << "Missing SHM extension"sv << std::endl;

      return -1;
    }

    auto iter = xcb_setup_roots_iterator(xcb_get_setup(xcb.get()));
    display = iter.data;
    seg = xcb_generate_id(xcb.get());

    shm_id.id = shmget(IPC_PRIVATE, frame_size(), IPC_CREAT | 0777);
    if(shm_id.id == -1) {
      std::cout << "shmget failed"sv << std::endl;
      return -1;
    }

    xcb_shm_attach(xcb.get(), seg, shm_id.id, false);
    data.data = shmat(shm_id.id, nullptr, 0);

    if ((uintptr_t)data.data == -1) {
      std::cout << "shmat failed"sv << std::endl;

      return -1;
    }

    return 0;
  }

  std::uint32_t frame_size() {
    return display->height_in_pixels * display->width_in_pixels * 4;
  }
};

struct mic_attr_t : public mic_t {
  pa_sample_spec ss;
  util::safe_ptr<pa_simple, pa_simple_free> mic;

  explicit mic_attr_t(const pa_sample_spec& ss) : ss(ss), mic {} {}
  std::vector<std::int16_t> sample(std::size_t sample_size) override {
    std::vector<std::int16_t> sample_buf;
    sample_buf.resize(sample_size);

    auto buf = sample_buf.data();
    int error;
    if(pa_simple_read(mic.get(), buf, sample_size * 2, &error)) {
      std::cout << "pa_simple_read() failed: "sv << pa_strerror(error) << std::endl;
    }

    return sample_buf;
  }
};

std::unique_ptr<display_t> shm_display() {
  auto shm = std::make_unique<shm_attr_t>();

  if(shm->init()) {
    return nullptr;
  }

  return shm;
}

std::unique_ptr<display_t> display() {
  auto shm_disp = shm_display();

  if(!shm_disp) {
    return std::unique_ptr<display_t> { new x11_attr_t {} };
  }

  return shm_disp;
}

//FIXME: Pass frame_rate instead of hard coding it
std::unique_ptr<mic_t> microphone() {
  std::unique_ptr<mic_attr_t> mic {
    new mic_attr_t {
      { PA_SAMPLE_S16LE, 48000, 2 }
    }
  };

  int error;
  mic->mic.reset(
    pa_simple_new(nullptr, "sunshine", pa_stream_direction_t::PA_STREAM_RECORD, nullptr, "sunshine_record", &mic->ss, nullptr, nullptr, &error)
  );

  if(!mic->mic) {
    auto err_str = pa_strerror(error);
    std::cout << "pa_simple_new() failed: "sv << err_str << std::endl;

    exit(1);
  }

  return mic;
}

ifaddr_t get_ifaddrs() {
  ifaddrs *p { nullptr };

  getifaddrs(&p);

  return ifaddr_t { p };
}

std::string from_sockaddr(const sockaddr *const ip_addr) {
  char data[INET6_ADDRSTRLEN];

  auto family = ip_addr->sa_family;
  if(family == AF_INET6) {
    inet_ntop(AF_INET6, &((sockaddr_in6*)ip_addr)->sin6_addr, data, INET6_ADDRSTRLEN);
  }

  if(family == AF_INET) {
    inet_ntop(AF_INET, &((sockaddr_in*)ip_addr)->sin_addr, data, INET_ADDRSTRLEN);
  }

  return std::string { data };
}

std::string get_local_ip(int family) {
  std::bitset<2> family_f {};

  if(family == 0) {
    family_f[0] = true;
    family_f[1] = true;
  }

  if(family == AF_INET) {
    family_f[0] = true;
  }

  if(family == AF_INET6) {
    family_f[1] = true;
  }


  std::string ip_addr;
  auto ifaddr = get_ifaddrs();
  for(auto pos = ifaddr.get(); pos != nullptr; pos = pos->ifa_next) {
    if(pos->ifa_addr && pos->ifa_flags & IFF_UP && !(pos->ifa_flags & IFF_LOOPBACK)) {
      if(
        (family_f[0] && pos->ifa_addr->sa_family == AF_INET) ||
        (family_f[1] && pos->ifa_addr->sa_family == AF_INET6)
        ){
        ip_addr = from_sockaddr(pos->ifa_addr);
        break;
      }
    }
  }

  return ip_addr;
}

std::string get_local_ip() { return get_local_ip(AF_INET); }

void freeImage(XImage *p) {
  XDestroyImage(p);
}
}