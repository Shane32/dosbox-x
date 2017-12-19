
#include "dosbox.h"
#include "setup.h"
#include "video.h"
#include "pic.h"
#include "vga.h"
#include "inout.h"
#include "programs.h"
#include "support.h"
#include "setup.h"
#include "timer.h"
#include "mem.h"
#include "util_units.h"
#include "control.h"
#include "pc98_cg.h"
#include "pc98_gdc.h"
#include "pc98_gdc_const.h"
#include "mixer.h"

#include <string.h>
#include <stdlib.h>
#include <string>
#include <stdio.h>

using namespace std;

void gdc_proc_schedule_delay(void);
void gdc_proc_schedule_cancel(void);
void gdc_proc_schedule_done(void);

PC98_GDC_state::PC98_GDC_state() {
    memset(param_ram,0,sizeof(param_ram));

    // make a display partition area to cover the screen, whatever it is.
    param_ram[0] = 0x00;        // SAD=0
    param_ram[1] = 0x00;        // SAD=0
    param_ram[2] = 0xF0;        // LEN=3FF
    param_ram[3] = 0x3F;        // LEN=3FF WD1=0

    display_partition_mask = 3;
    doublescan = false;
    param_ram_wptr = 0;
    display_partition = 0;
    row_line = 0;
    row_height = 16;
    scan_address = 0;
    current_command = 0xFF;
    proc_step = 0xFF;
    display_enable = true;
    display_mode = 0;
    cursor_enable = true;
    cursor_blink_state = 0;
    cursor_blink_count = 0;
    cursor_blink_rate = 0x20;
    video_framing = 0;
    master_sync = false;
    draw_only_during_retrace = 0;
    dynamic_ram_refresh = 0;
    cursor_blink = true;
    idle = false;
    reset_fifo();
    reset_rfifo();
}

size_t PC98_GDC_state::fifo_can_read(void) {
    return fifo_write - fifo_read;
}

void PC98_GDC_state::take_reset_sync_parameters(void) {
    /* P1 = param[0] = 0 0 C F I D G S
     *  CG = [1:0] = display mode
     *  IS = [1:0] = video framing
     *   F = drawing time window
     *   D = dynamic RAM refresh cycles enable */
    draw_only_during_retrace =      !!(cmd_parm_tmp[0] & 0x10); /* F */
    dynamic_ram_refresh =           !!(cmd_parm_tmp[0] & 0x04); /* D */
    display_mode = /* CG = [1:0] */
        ((cmd_parm_tmp[0] & 0x20) ? 2 : 0) +
        ((cmd_parm_tmp[0] & 0x02) ? 1 : 0);
    video_framing = /* IS = [1:0] */
        ((cmd_parm_tmp[0] & 0x08) ? 2 : 0) +
        ((cmd_parm_tmp[0] & 0x01) ? 1 : 0);

    /* P2 = param[1] = AW = active display words per line - 2. must be even number. */
    display_pitch = active_display_words_per_line = (uint16_t)cmd_parm_tmp[1] + 2u;

    /* P3 = param[2] =
     *   VS(L)[2:0] = [7:5] = low bits of VS
     *   HS = [4:0] = horizontal sync width - 1 */
    horizontal_sync_width = (cmd_parm_tmp[2] & 0x1F) + 1;
    vertical_sync_width = (cmd_parm_tmp[2] >> 5);

    /* P4 = param[3] =
     *   HFP = [7:2] = horizontal front porch width - 1
     *   VS(H)[4:3] = [1:0] = high bits of VS
     *
     *   VS = vertical sync width */
    vertical_sync_width += (cmd_parm_tmp[3] & 3) << 3;
    horizontal_front_porch_width = (cmd_parm_tmp[3] >> 2) + 1;

    /* P5 = param[4] =
     *   0 = [7:6] = 0
     *   HBP = [5:0] = horizontal back porch width - 1 */
    horizontal_back_porch_width = (cmd_parm_tmp[4] & 0x3F) + 1;

    /* P6 = param[5] =
     *   0 = [7:6] = 0
     *   VFP = [5:0] = vertical front porch width */
    vertical_front_porch_width = (cmd_parm_tmp[5] & 0x3F);

    /* P7 = param[6] =
     *   AL(L)[7:0] = [7:0] = Active Display Lines per video field, low bits */
    active_display_lines = (cmd_parm_tmp[6] & 0xFF);

    /* P8 = parm[7] =
     *   VBP = [7:2] = vertical back porch width
     *   AL(H)[9:8] = [1:0] = Active Display Lines per video field, high bits */
    active_display_lines += (cmd_parm_tmp[7] & 3) << 8;
    vertical_back_porch_width = cmd_parm_tmp[7] >> 2;

    LOG_MSG("GDC: RESET/SYNC DOOR=%u DRAM=%u DISP=%u VFRAME=%u AW=%u HS=%u VS=%u HFP=%u HBP=%u VFP=%u AL=%u VBP=%u",
        draw_only_during_retrace?1:0,
        dynamic_ram_refresh?1:0,
        display_mode,
        video_framing,
        active_display_words_per_line,
        horizontal_sync_width,
        vertical_sync_width,
        horizontal_front_porch_width,
        horizontal_back_porch_width,
        vertical_front_porch_width,
        active_display_lines,
        vertical_back_porch_width);

    VGA_StartResize();
}

void PC98_GDC_state::cursor_advance(void) {
    cursor_blink_count++;
    if (cursor_blink_count == cursor_blink_rate) {
        cursor_blink_count = 0;

        if ((++cursor_blink_state) >= 4)
            cursor_blink_state = 0;
    }
    else if (cursor_blink_count & 0x40) {
        cursor_blink_count = 0;
    }
}

void PC98_GDC_state::take_cursor_pos(unsigned char bi) {
    /* P1 = param[0] = EAD(L) = address[7:0]
     *
     * P2 = param[1] = EAD(M) = address[15:0]
     *
     * P3 = param[2]
     *   dAD = [7:4] = Dot address within the word
     *   0 = [3:2] = 0
     *   EAD(H) = [1:0] = address[17:16] */
    if (bi == 1) {
		vga.config.cursor_start &= ~(0xFF << 0);
		vga.config.cursor_start |=  cmd_parm_tmp[0] << 0;
    }
    else if (bi == 2) {
		vga.config.cursor_start &= ~(0xFF << 8);
		vga.config.cursor_start |=  cmd_parm_tmp[1] << 8;
    }
    else if (bi == 3) {
		vga.config.cursor_start &= ~(0x03 << 16);
		vga.config.cursor_start |=  (cmd_parm_tmp[2] & 3) << 16;

        // TODO: "dot address within the word"
    }
}

void PC98_GDC_state::take_cursor_char_setup(unsigned char bi) {
    /* P1 = param[0] =
     *   DC = [7:7] = display cursor if set
     *   0 = [6:5] = 0
     *   LR = [4:0] = lines per character row - 1 */
    if (bi == 1) {
        cursor_enable = !!(cmd_parm_tmp[0] & 0x80);

		vga.crtc.maximum_scan_line = cmd_parm_tmp[0] & 0x1F;
		vga.draw.address_line_total = vga.crtc.maximum_scan_line + 1;
    }

    /* P2 = param[1] =
     *   BR[1:0] = [7:6] = blink rate
     *   SC = [5:5] = 1=steady cursor  0=blinking cursor
     *   CTOP = [4:0] = cursor top line number in the row */

    /* P3 = param[2] =
     *   CBOT = [7:3] = cursor bottom line number in the row CBOT < LR
     *   BR[4:2] = [2:0] = blink rate */
    if (bi == 3) {
        cursor_blink_rate  = (cmd_parm_tmp[1] >> 6) & 3;
        cursor_blink_rate += (cmd_parm_tmp[2] & 7) << 2;
        if (cursor_blink_rate == 0) cursor_blink_rate = 0x20;
        cursor_blink_rate *= 2;

        cursor_blink = !(cmd_parm_tmp[1] & 0x20);

		vga.crtc.cursor_start = (cmd_parm_tmp[1] & 0x1F);
		vga.draw.cursor.sline = vga.crtc.cursor_start;

		vga.crtc.cursor_end   = (cmd_parm_tmp[2] >> 3) & 0x1F;
		vga.draw.cursor.eline = vga.crtc.cursor_end;
    }

    /* blink-on time + blink-off time = 2 x BR (video frames).
     * attribute blink rate is 3/4 on 1/4 off duty cycle.
     * for interlaced graphics modes, set BR[1:0] = 3 */
}

void PC98_GDC_state::idle_proc(void) {
    Bit16u val;

    if (fifo_empty())
        return;

    val = read_fifo();
    if (val & 0x100) { // command
        current_command = val & 0xFF;
        proc_step = 0;

        switch (current_command) {
            case GDC_CMD_RESET: // 0x00         0 0 0 0 0 0 0 0
                LOG_MSG("GDC: reset");
                display_enable = false;
                idle = true;
                reset_fifo();
                reset_rfifo();
                break;
            case GDC_CMD_DISPLAY_BLANK:  // 0x0C   0 0 0 0 1 1 0 DE
            case GDC_CMD_DISPLAY_BLANK+1:// 0x0D   DE=display enable
                display_enable = !!(current_command & 1); // bit 0 = display enable
                current_command &= ~1;
                break;
            case GDC_CMD_SYNC:  // 0x0E         0 0 0 0 0 0 0 DE
            case GDC_CMD_SYNC+1:// 0x0F         DE=display enable
                display_enable = !!(current_command & 1); // bit 0 = display enable
                current_command &= ~1;
                LOG_MSG("GDC: sync");
                break;
            case GDC_CMD_PITCH_SPEC:          // 0x47        0 1 0 0 0 1 1 1
                break;
            case GDC_CMD_CURSOR_POSITION:     // 0x49        0 1 0 0 1 0 0 1
                LOG_MSG("GDC: cursor pos");
                break;
            case GDC_CMD_CURSOR_CHAR_SETUP:   // 0x4B        0 1 0 0 1 0 1 1
                LOG_MSG("GDC: cursor setup");
                break;
            case GDC_CMD_START_DISPLAY:       // 0x6B        0 1 1 0 1 0 1 1
                idle = false;
                break;
            case GDC_CMD_VERTICAL_SYNC_MODE:  // 0x6E        0 1 1 0 1 1 1 M
            case GDC_CMD_VERTICAL_SYNC_MODE+1:// 0x6F        M=generate and output vertical sync (0=or else accept external vsync)
                master_sync = !!(current_command & 1);
                current_command &= ~1;
                LOG_MSG("GDC: vsyncmode master=%u",master_sync);
                break;
            case GDC_CMD_PARAMETER_RAM_LOAD:   // 0x70       0 1 1 1 S S S S
            case GDC_CMD_PARAMETER_RAM_LOAD+1: // 0x71       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+2: // 0x72       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+3: // 0x73       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+4: // 0x74       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+5: // 0x75       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+6: // 0x76       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+7: // 0x77       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+8: // 0x78       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+9: // 0x79       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+10:// 0x7A       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+11:// 0x7B       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+12:// 0x7C       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+13:// 0x7D       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+14:// 0x7E       S=starting byte in parameter RAM
            case GDC_CMD_PARAMETER_RAM_LOAD+15:// 0x7F       S=starting byte in parameter RAM
                param_ram_wptr = current_command & 0xF;
                current_command = GDC_CMD_PARAMETER_RAM_LOAD;
                break;
            default:
                LOG_MSG("GDC: Unknown command 0x%x",current_command);
                break;
        };
    }
    else {
        /* parameter parsing */
        switch (current_command) {
            /* RESET and SYNC take the same 8 byte parameters */
            case GDC_CMD_RESET:
            case GDC_CMD_SYNC:
                if (proc_step < 8) {
                    cmd_parm_tmp[proc_step] = (uint8_t)val;
                    if ((++proc_step) == 8) {
                        take_reset_sync_parameters();
                    }
                }
                break;
            case GDC_CMD_PITCH_SPEC:
                if (proc_step < 1)
                    display_pitch = (val != 0) ? val : 0x100;
                break;
            case GDC_CMD_CURSOR_POSITION:
                if (proc_step < 3) {
                    cmd_parm_tmp[proc_step++] = (uint8_t)val;
                    take_cursor_pos(proc_step);
                }
                break;
            case GDC_CMD_CURSOR_CHAR_SETUP:
                if (proc_step < 3) {
                    cmd_parm_tmp[proc_step++] = (uint8_t)val;
                    if (proc_step == 1 || proc_step == 3) {
                        take_cursor_char_setup(proc_step);
                    }
                }
                break;
            case GDC_CMD_PARAMETER_RAM_LOAD:
                param_ram[param_ram_wptr] = (uint8_t)val;
                if ((++param_ram_wptr) >= 16) param_ram_wptr = 0;
                break;
        };
    }

    if (!fifo_empty())
        gdc_proc_schedule_delay();
}

bool PC98_GDC_state::fifo_empty(void) {
    return (fifo_read >= fifo_write);
}

Bit16u PC98_GDC_state::read_fifo(void) {
    Bit16u val;

    val = fifo[fifo_read];
    if (fifo_read < fifo_write)
        fifo_read++;

    return val;
}

void PC98_GDC_state::next_line(void) {
    if ((++row_line) == row_height) {
        scan_address += display_pitch;
        row_line = 0;
    }
    else if (row_line & 0x20) {
        row_line = 0;
    }

    if (--display_partition_rem_lines == 0) {
        next_display_partition();
        load_display_partition();
    }
}

void PC98_GDC_state::begin_frame(void) {
    row_line = 0;
    scan_address = 0;
    display_partition = 0;

    /* the actual starting address is determined by the display partition in paramter RAM */
    load_display_partition();
}

void PC98_GDC_state::load_display_partition(void) {
    unsigned char *pram = param_ram + (display_partition * 4);

    scan_address  =  pram[0];
    scan_address +=  pram[1]         << 8;
    scan_address += (pram[2] & 0x03) << 16;

    display_partition_rem_lines  =  pram[2]         >> 4;
    display_partition_rem_lines += (pram[3] & 0x3F) << 4;
    if (display_partition_rem_lines == 0)
        display_partition_rem_lines = 0x400;

    if (master_sync) { /* character mode */
    /* RAM+0 = SAD1 (L)
     *
     * RAM+1 = 0 0 0 SAH1 (M) [4:0]
     *
     * RAM+2 = LEN1 (L) [7:4]  0 0 0 0
     *
     * RAM+3 = WD1 0 LEN1 (H) [5:0] */
        scan_address &= 0x1FFF;
    }
    else { /* graphics mode */
    /* RAM+0 = SAD1 (L)
     *
     * RAM+1 = SAH1 (M)
     *
     * RAM+2 = LEN1 (L) [7:4]  0 0   SAD1 (H) [1:0]
     *
     * RAM+3 = WD1 IM LEN1 (H) [5:0] */
    }
}

void PC98_GDC_state::force_fifo_complete(void) {
    while (!fifo_empty())
        idle_proc();
}

void PC98_GDC_state::next_display_partition(void) {
    display_partition = (display_partition + 1) & display_partition_mask;
}

void PC98_GDC_state::reset_fifo(void) {
    fifo_read = fifo_write = 0;
}

void PC98_GDC_state::reset_rfifo(void) {
    rfifo_read = rfifo_write = 0;
}

void PC98_GDC_state::flush_fifo_old(void) {
    if (fifo_read != 0) {
        unsigned int sz = (fifo_read <= fifo_write) ? (fifo_write - fifo_read) : 0;

        for (unsigned int i=0;i < sz;i++)
            fifo[i] = fifo[i+fifo_read];

        fifo_read = 0;
        fifo_write = sz;
    }
}

bool PC98_GDC_state::write_fifo(const uint16_t c) {
    if (fifo_write >= PC98_GDC_FIFO_SIZE)
        flush_fifo_old();
    if (fifo_write >= PC98_GDC_FIFO_SIZE)
        return false;

    fifo[fifo_write++] = c;
    gdc_proc_schedule_delay();
    return true;
}

bool PC98_GDC_state::write_fifo_command(const unsigned char c) {
    return write_fifo(c | GDC_COMMAND_BYTE);
}

bool PC98_GDC_state::write_fifo_param(const unsigned char c) {
    return write_fifo(c);
}

bool PC98_GDC_state::rfifo_has_content(void) {
    return (rfifo_read < rfifo_write);
}

uint8_t PC98_GDC_state::read_status(void) {
    double timeInFrame = PIC_FullIndex()-vga.draw.delay.framestart;
    double timeInLine=fmod(timeInFrame,vga.draw.delay.htotal);
    uint8_t ret;

    ret  = 0x00; // light pen not present

	if (timeInFrame >= vga.draw.delay.vdend) {
        ret |= 0x40; // vertical blanking
    }
    else {
        if (timeInLine >= vga.draw.delay.hblkstart && 
            timeInLine <= vga.draw.delay.hblkend)
            ret |= 0x40; // horizontal blanking
    }

    if (timeInFrame >= vga.draw.delay.vrstart &&
        timeInFrame <= vga.draw.delay.vrend)
        ret |= 0x20; // vertical retrace

    // TODO: 0x10 bit 4 DMA execute

    // TODO: 0x08 bit 3 drawing in progress

    if (fifo_write >= PC98_GDC_FIFO_SIZE)
        flush_fifo_old();

    if (fifo_read == fifo_write)
        ret |= 0x04; // FIFO empty
    if (fifo_write >= PC98_GDC_FIFO_SIZE)
        ret |= 0x02; // FIFO full
    if (rfifo_has_content())
        ret |= 0x01; // data ready

    return ret;
}

uint8_t PC98_GDC_state::rfifo_read_data(void) {
    uint8_t ret;

    ret = rfifo[rfifo_read];
    if (rfifo_read < rfifo_write) {
        if (++rfifo_read >= rfifo_write) {
            rfifo_read = rfifo_write = 0;
            rfifo[0] = ret;
        }
    }

    return ret;
}
