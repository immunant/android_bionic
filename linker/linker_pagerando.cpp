/*
 * Copyright (C) 2018 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "linker_pagerando.h"

#include "linker_debug.h"
#include "linker_globals.h"

#include <android-base/file.h>

#include <errno.h>
#include <sys/mman.h>


static ElfW(Addr) g_pot_base = 0;

ElfW(Addr) get_pot_base() {
  if (!g_pot_base) {
    // Reserve address space for the full table
    void* map_base = mmap(reinterpret_cast<void*>(get_random_address()),
                          kPOTSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map_base == MAP_FAILED) {
      return 0;
    }
    g_pot_base = reinterpret_cast<ElfW(Addr)>(map_base);
  }
  return g_pot_base;
}


// Tentative pagerando mapping ranges. Only implemented for ARM, AArch64. These
// guards must match the guards for picking a random address below.
#if defined(__arm__)
static const unsigned long RAND_ADDR_LOW  = 0xb0000000;
static const unsigned long RAND_ADDR_HIGH = 0xb6000000;
#elif defined(__aarch64__)
static const unsigned long RAND_ADDR_LOW  = 0x1000000000;
static const unsigned long RAND_ADDR_HIGH = 0x5000000000;
#endif

ElfW(Addr) get_random_address() {
  ElfW(Addr) random_start_address;
  ElfW(Addr) range = RAND_ADDR_HIGH - RAND_ADDR_LOW;

  // 2^N % x == (2^N - x) % x where N = 32 or 64
  ElfW(Addr) min = -range % range;

  // Calculate a random value in the range [2^N % range, 2^N) where N = 32
  // or 64 depending on the target address size.
  do {
    // This is actually a ChaCha RNG seeded with getentropy(), so is
    // reasonable for secure randomness. We use random_buf to get a 64-bit
    // value on 64-bit platforms.
    arc4random_buf(&random_start_address, sizeof(ElfW(Addr)));
  } while (random_start_address < min);
  // Map that random number back to the range [RAND_ADDR_LOW, RAND_ADDR_HIGH)
  random_start_address = random_start_address % range + RAND_ADDR_LOW;
  return random_start_address;
}

#if 0
// Pull the POT index out of the dynamic table. NOT IMPLEMENTED IN LINKER YET.
// TODO: Make the linker or a post-processing step add this entry.
unsigned get_pot_index(ElfW(Dyn)* dynamic) {
  for (ElfW(Dyn)* d = dynamic; d->d_tag != DT_NULL; ++d) {
    if (d->d_tag == DT_ANDROID_POTINDEX)
      return d->d_un.d_val;
  }
}
#endif


static constexpr const char* kPOTMapPath = "/system/etc/ld.pot_map.txt";

class POTIndexMap {
public:
  POTIndexMap() : initialized_(false) { }

  bool read_pot_map(const char *pot_map_path);

  size_t get_pot_index(const std::string &soname);

private:
  std::unordered_map<std::string, size_t> pot_indices_;
  bool initialized_;
};


bool POTIndexMap::read_pot_map(const char *pot_map_path = kPOTMapPath) {
  std::string content;
  if (!android::base::ReadFileToString(pot_map_path, &content)) {
    DL_ERR("error reading pot map file \"%s\": %s",
           pot_map_path, strerror(errno));
    return false;
  }

  std::string line;
  size_t index = 0;
  size_t pos = 0, next_newline;
  do {
    next_newline = content.find('\n', pos);
    if (next_newline == std::string::npos) {
      line = content.substr(pos);
    } else {
      line = content.substr(pos, next_newline - pos);
    }

    DEBUG("Assigning %s to index %zu", line.c_str(), index);
    pot_indices_[line] = index;

    pos = next_newline + 1;
    ++index;
  } while (next_newline != std::string::npos);

  return true;
}


size_t POTIndexMap::get_pot_index(const std::string &soname) {
  if (!initialized_) {
    if (!read_pot_map()) {
      return kPOTIndexError;
    }

    initialized_ = true;
  }

  auto it = pot_indices_.find(soname);
  if (it == pot_indices_.end()) {
    return kPOTIndexError;
  }

  return it->second;
}

static POTIndexMap g_pot_map;

size_t get_pot_index(const std::string &soname) {
  return g_pot_map.get_pot_index(soname);
}
