// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
//  PixelPusher protocol implementation for LED matrix
//
//  Copyright (C) 2013 Henner Zeller <h.zeller@acm.org>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "led-strip.h"
#include "multi-spi.h"

#include "pp-server.h"

static const int kMaxUDPPacketSize = 65507;  // largest practical w/ IPv4 header
static const int kDefaultUDPPacketSize = 1460;

typedef spixels::LEDStrip *(*StripFactory)(spixels::MultiSPI *spi, int connector, int count);

// Adapter of Spixels operated LED strips to pp::OutputDevice
class SpixelsDevice : public pp::OutputDevice {
public:
  SpixelsDevice(int spi_mhz,
                StripFactory strip_factory, int num_strips, int strip_len)
    : num_strips_(num_strips),
      strip_len_(strip_len),
      strips_(new spixels::LEDStrip* [ num_strips_ ]),
      spi_(spixels::CreateDirectMultiSPI(spi_mhz)) {
    for (int i = 0; i < num_strips_; ++i) {
      strips_[i] = strip_factory(
        spi_, spixels::MultiSPI::SPIPinForConnector(i+1), strip_len_);
    }
  }

  ~SpixelsDevice() {
    for (int i = 0; i < num_strips_; ++i) delete strips_[i];
    delete [] strips_;
    delete spi_;
  }

  virtual int num_strips() const { return num_strips_; }
  virtual int num_pixel_per_strip() const { return strip_len_; }

  virtual void SetPixel(int strip, int pixel,
                        const ::pp::PixelColor &col) {
    if (strip < 0 || strip >= num_strips_) return;
    strips_[strip]->SetPixel(pixel, col.red, col.green, col.blue);
  }

  virtual void FlushFrame() {
    spi_->SendBuffers();
  }

private:
  const int num_strips_;
  const int strip_len_;
  spixels::LEDStrip **strips_;
  spixels::MultiSPI *spi_;
};

static int usage(const char *progname) {
  fprintf(stderr, "usage: %s <options>\n", progname);
  fprintf(stderr, "Options:\n"
          "\t-T <type>     : One of APA102, LPD6803, LPD8806, WS2801; default: APA102\n"
          "\t-c <clockspeed>: SPI clock-speed in Mhz [1..15]. Default: 4\n"
          "\t-S <strips>   : Number of connected LED strips (default: 16)\n"
          "\t-L <len>      : Length of LED strips (default: 144)\n"
          "\t-i <iface>    : network interface, such as eth0, wlan0. "
          "Default eth0\n"
          "\t-G <group>    : PixelPusher group (default: 0)\n"
          "\t-C <controller> : PixelPusher controller (default: 0)\n"
          "\t-a <artnet-universe,artnet-channel>: if used with artnet. Default 0,0\n"
          "\t-u <udp-size> : Max UDP data/packet (default %d)\n"
          "\t                Best use the maximum that works with your network (up to %d).\n",
          kDefaultUDPPacketSize, kMaxUDPPacketSize);

  return 1;
}

static StripFactory parseType(const char *type) {
  if (strcasecmp(type, "APA102") == 0) return &spixels::CreateAPA102Strip;
  if (strcasecmp(type, "WS2801") == 0) return &spixels::CreateWS2801Strip;
  if (strcasecmp(type, "LPD6803") == 0) return &spixels::CreateLPD6803Strip;
  if (strcasecmp(type, "LPD8806") == 0) return &spixels::CreateLPD8806Strip;
  return NULL;
}

int main(int argc, char *argv[]) {
  pp::PPOptions pp_options;
  pp_options.artnet_universe = -1;
  pp_options.artnet_channel = -1;
  pp_options.network_interface = "eth0";
  int num_strips = 16;
  int strip_len = 144;
  int SPI_clock_mhz = 4;

  StripFactory strip_creator = &spixels::CreateAPA102Strip;

  int opt;
  while ((opt = getopt(argc, argv, "S:L:i:u:a:G:C:T:c:")) != -1) {
    switch (opt) {
    case 'T':
      strip_creator = parseType(optarg);
      break;
    case 'c':
      SPI_clock_mhz = atoi(optarg);
      break;
    case 'S':
      num_strips = atoi(optarg);
      break;
    case 'L':
      strip_len = atoi(optarg);
      break;
    case 'i':
      pp_options.network_interface = strdup(optarg);
      break;
    case 'u':
      pp_options.udp_packet_size = atoi(optarg);
      break;
    case 'G':
      pp_options.group = atoi(optarg);
      break;
    case 'C':
      pp_options.controller = atoi(optarg);
      break;
    case 'a':
      if (2 != sscanf(optarg, "%d,%d",
                      &pp_options.artnet_universe, &pp_options.artnet_channel)) {
        fprintf(stderr, "Artnet parameters must be <universe>,<channel>\n");
        return 1;
      }
      break;
    default:
      return usage(argv[0]);
    }
  }

  if (strip_creator == NULL) {
    fprintf(stderr, "Invalid type of LED strip given with -T\n");
    return 1;
  }
  if (SPI_clock_mhz < 1 || SPI_clock_mhz > 15) {
    fprintf(stderr, "SPI clock speed out of range [1..15]\n");
    return 1;
  }

  // Some parameter checks.
  if (getuid() != 0) {
    fprintf(stderr, "Must run as root to be able to access /dev/mem\n"
            "Prepend 'sudo' to the command:\n\tsudo %s ...\n", argv[0]);
    return 1;
  }

  SpixelsDevice pixel_strips(SPI_clock_mhz,
                             strip_creator, num_strips, strip_len);
  if (!pp::StartPixelPusherServer(pp_options, &pixel_strips)) {
    return 1;
  }

  for(;;) sleep(INT_MAX);
  pp::ShutdownPixelPusherServer();

  return 0;
}
