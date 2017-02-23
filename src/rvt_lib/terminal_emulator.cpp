#include <iostream>
/*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
*   Product name: redemption, a FLOSS RDP proxy
*   Copyright (C) Wallix 2010-2016
*   Author(s): Jonathan Poelen
*/

#include "terminal_emulator.hpp"

#include "rvt/character_color.hpp"
#include "rvt/vt_emulator.hpp"
#include "rvt/utf8_decoder.hpp"
#include "rvt/text_rendering.hpp"

#include <cerrno>
#include <cstdlib>

#include <unistd.h> // unlink
#include <stdio.h> // rename
#include <fcntl.h> // O_* flags
#include <sys/stat.h> // fchmod


struct TerminalEmulator
{
    rvt::VtEmulator emulator;
    rvt::Utf8Decoder decoder;

    TerminalEmulator(int lines, int columns)
    : emulator(lines, columns)
    {}
};


static ssize_t write_all(int fd, const void * data, size_t len) noexcept
{
    size_t remaining_len = len;
    size_t total_sent = 0;
    while (remaining_len) {
        ssize_t ret = ::write(fd, static_cast<const char*>(data) + total_sent, remaining_len);
        if (ret <= 0){
            if (errno == EINTR){
                continue;
            }
            return -1;
        }
        remaining_len -= ret;
        total_sent += ret;
    }
    return total_sent;
}

static int build_json_string(TerminalEmulator & emu, std::string & out) noexcept
{
    try {
        out = rvt::json_rendering(
            emu.emulator.getWindowTitle(),
            emu.emulator.getCurrentScreen(),
            rvt::color_table
        );
    }
    catch (...) {
        return -1;
    }

    return 0;
}


extern "C" {

#define return_if(x) do { if (x) { return -1; } } while (0)
#define return_errno_if(x) do { if (x) { return errno ? errno : -1; } } while (0)

TerminalEmulator * terminal_emulator_init(int lines, int columns)
{
    return new TerminalEmulator(lines, columns);
}

int terminal_emulator_deinit(TerminalEmulator * emu)
{
    delete emu;
    return 0;
}

int terminal_emulator_finish(TerminalEmulator * emu)
{
    return_if(!emu);

    auto send_fn = [emu](rvt::ucs4_char ucs) { emu->emulator.receiveChar(ucs); };
    emu->decoder.end_decode(send_fn);
    return 0;
}


int terminal_emulator_set_title(TerminalEmulator* emu, char const * title)
{
    return_if(!emu);

    rvt::ucs4_char ucs_title[255];
    rvt::ucs4_char * p = std::begin(ucs_title);
    rvt::ucs4_char * e = std::end(ucs_title) - 4;

    std::size_t sz = strlen(title);

    auto send_fn = [&p](rvt::ucs4_char ucs) { *p = ucs; ++p; };
    while (sz && p < e) {
        std::size_t consumed = std::min(sz, std::size_t(e-p));
        emu->decoder.decode(const_bytes_array(title, consumed), send_fn);
        sz -= consumed;
    }
    emu->decoder.end_decode(send_fn);

    emu->emulator.setWindowTitle({ucs_title, std::size_t(e-ucs_title)});
    return 0;
}

int terminal_emulator_set_log_function(TerminalEmulator * emu, void(*log_func)(char const*))
{
    return_if(!emu);

    emu->emulator.setLogFunction(log_func);
    return 0;
}

int terminal_emulator_set_log_function_ctx(TerminalEmulator * emu, void(*log_func)(void *, char const *), void * ctx)
{
    return_if(!emu);

    emu->emulator.setLogFunction([log_func, ctx](char const * s) { log_func(ctx, s); });
    return 0;
}

int terminal_emulator_feed(TerminalEmulator * emu, char const * s, int n)
{
    return_if(!emu);

    auto send_fn = [emu](rvt::ucs4_char ucs) { emu->emulator.receiveChar(ucs); };
    emu->decoder.decode(const_bytes_array(s, std::max(n, 0)), send_fn);
    return 0;
}

int terminal_emulator_resize(TerminalEmulator * emu, int lines, int columns)
{
    return_if(!emu);

    emu->emulator.setScreenSize(lines, columns);
    return 0;
}

int terminal_emulator_write(TerminalEmulator * emu, char const * filename, int mode)
{
    return_if(!emu);

    std::string out;
    return_errno_if(build_json_string(*emu, out));

    int fd = ::open(filename, O_WRONLY | O_CREAT, mode);
    return_errno_if(fd == -1);

    if (write_all(fd, out.c_str(), out.size()) != static_cast<ssize_t>(out.size())) {
        auto err = errno;
        close(fd);
        unlink(filename);
        return err ? err : -1;
    }

    close(fd);

    return 0;
}

int terminal_emulator_write_integrity(
    TerminalEmulator * emu,
    char const * filename,
    char const * prefix_tmp_filename,
    int mode
) {
    return_if(!emu);

    std::string out;
    return_errno_if(build_json_string(*emu, out));

    char tmpfilename[4096];
    tmpfilename[0] = 0;
    int n = std::snprintf(tmpfilename, utils::size(tmpfilename) - 1, "%s-teremu-XXXXXX.tmp", prefix_tmp_filename);
    tmpfilename[n < 0 ? 0 : n] = 0;

    const int fd = ::mkostemps(tmpfilename, 4, O_WRONLY | O_CREAT);
    return_errno_if(fd == -1);

    if (fchmod(fd, mode) == -1
     || write_all(fd, out.c_str(), out.size()) != static_cast<ssize_t>(out.size())
     || rename(tmpfilename, filename) == -1
    ) {
        auto const err = errno;
        close(fd);
        unlink(filename);
        return err ? err : -1;
    }

    close(fd);

    return 0;
}

}
