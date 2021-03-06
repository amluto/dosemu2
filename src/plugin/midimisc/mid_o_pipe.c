/*
 *  Copyright (C) 2006 Stas Sergeev <stsp@users.sourceforge.net>
 *
 * The below copyright strings have to be distributed unchanged together
 * with this file. This prefix can not be modified or separated.
 */

/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include "emu.h"
#include "dosemu_config.h"
#include "init.h"
#include "sound/midi.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>


static int pipe_fd = -1;
#define midopipe_name "MIDI Output: named pipe"

static int midopipe_init(void *arg)
{
    char *name = DOSEMU_MIDI_PATH;
    mkfifo(name, 0666);
    pipe_fd = RPT_SYSCALL(open(name, O_WRONLY | O_NONBLOCK));
    if (pipe_fd == -1) {
	int err = errno;
	S_printf("%s: unable to open %s for writing (%s)%s\n", midopipe_name, name,
	    strerror(errno), errno == ENXIO ? ", will continue trying" : "");
	if (err == ENXIO) { /* no FIFO readers */
	    return 1;
	} else { /* some other problem */
	   return 0;
	}
    }
    /* open ok */
    return 1;
}

static void midopipe_done(void *arg)
{
    if (pipe_fd == -1)
	return;
    close(pipe_fd);
    pipe_fd = -1;
}

static void midopipe_write(unsigned char val)
{
    /* Try again to open FIFO on each write in case some readers showed up. */
    if (pipe_fd == -1) {
	pipe_fd = RPT_SYSCALL(open(DOSEMU_MIDI_PATH, O_WRONLY | O_CREAT | O_NONBLOCK, 0666));
	if (pipe_fd == -1) {
	    return;
	}
    }
    if (write(pipe_fd, &val, 1) == -1) {
	error("MIDI: Error writing to %s, resetting: %s\n", midopipe_name, strerror(errno));
	close(pipe_fd);
	pipe_fd = -1;
    }
}

static const struct midi_out_plugin midopipe = {
    .name = midopipe_name,
    .open = midopipe_init,
    .close = midopipe_done,
    .write = midopipe_write,
    .flags = PCM_F_PASSTHRU,
};

CONSTRUCTOR(static void midopipe_register(void))
{
    midi_register_output_plugin(&midopipe);
}
