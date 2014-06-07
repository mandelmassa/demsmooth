/***************************************************************************
 *
 * Copyright (c) 2014 Mathias Thore
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "demo.h"

#define DEMSMOOTH_VERSION_MAJOR 1
#define DEMSMOOTH_VERSION_MINOR 1

/***************************************************************************
 * CONFIGURATION
 ***************************************************************************/

#define CAMERA_SMOOTH_SIZE           60

#define MOTION_SMOOTH_SIZE           30
#define MOTION_SMOOTH_RESTART_LIMIT  200

#define ROLL_TARGET                  10
#define ROLL_TRIGGER_ANGLE           0.3
#define ROLL_SPEED                   0.2

/***************************************************************************
 * MACROS
 ***************************************************************************/

#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define ABS(a, b) (((a) > (b) ? (a) - (b) : (b) - (a)))

/***************************************************************************
 * DATA TYPES
 ***************************************************************************/

typedef struct {
    block *b;
    float x;
    float y;
    float z;
} angle_info;

typedef struct {
    block *b;
    int16_t x;
    int16_t y;
    int16_t z;
    int16_t *x_ptr;
    int16_t *y_ptr;
    int16_t *z_ptr;
} location_info;

/***************************************************************************
 * FORWARD REFERENCES
 ***************************************************************************/

static void smooth_camera_xy(demo *demo);
static void smooth_camera_z(demo *demo);
static void smooth_motion(demo *demo);
static void add_roll(demo *demo);

static char *mystrduprep(char *in, char *pattern, char *rep);

static int block_is_timed(block *b);
static int count_setbits(uint32_t mask);
static int location_distance(location_info a, location_info b);
static int get_next_angle(angle_info *a);
static int get_next_location(uint16_t camera, location_info *loc,
                             location_info *prev);

/***************************************************************************
 * MAIN
 ***************************************************************************/

int main(int argc, char *argv[])
{
    demo *demo;
    char *outname;
    char *inname;
    dret_t dret;
    flagfield readflags[]  = {{READFLAG_FILENAME, NULL},
                              {READFLAG_END, READFLAG_END}};
    flagfield writeflags[] = {{WRITEFLAG_FILENAME, NULL},
                              {WRITEFLAG_REPLACE, NULL},
                              {WRITEFLAG_END, WRITEFLAG_END}};

    // find demo name parameter
    if (argc < 2) {
        printf("demsmooth %d.%02d by Mandel 2014\n"
               "usage:\n"
               "\n"
               "  demsmooth.exe <demoname.dem>\n"
               "\n"
               "will produce <demoname>_processed.dem\n",
               DEMSMOOTH_VERSION_MAJOR, DEMSMOOTH_VERSION_MINOR);
        
        exit(1);
    }
    inname = argv[1];

    // open demo
    readflags[0].value = inname;
    if ((dret = demo_read(readflags, &demo)) != DEMO_OK) {
        printf("demo not opened: %s\n", demo_error(dret));
        exit(1);
    }

    // out name
    outname = mystrduprep(inname, ".dem", "_processed.dem");
    if (outname == NULL) {
        printf("could not create valid out filename from %s\n", inname);
        exit(1);
    }

    // process demo
    smooth_motion(demo);
    smooth_camera_xy(demo);
    add_roll(demo);
    smooth_camera_z(demo);

    // write new demo
    writeflags[0].value = outname;
    if ((dret = demo_write(writeflags, demo)) != DEMO_OK) {
        printf("demo not written: %s\n", demo_error(dret));
    }
    else {
        printf("wrote %s\n", outname);
    }

    // cleanup
    demo_free(demo);
    free(outname);

    return 0;
}

/***************************************************************************
 * UTILITY
 ***************************************************************************/

static char *mystrduprep(char *in, char *pattern, char *rep)
{
    char *pos;
    char *str;

    if ((pos = strstr(in, pattern)) == NULL) {
        return NULL;
    }

    str = malloc(strlen(in) + strlen(rep) + 1);
    strcpy(str, in);
    pos = str + (pos - in);
    strcpy(pos, rep);

    return str;
}

static int block_is_timed(block *b)
{
    if (b->messages == NULL || b->messages->type != TIME) {
        return 0;
    }
    return 1;
}

static int count_setbits(uint32_t mask)
{
  int count;
  for (count = 0; mask; count++) {
    mask &= mask - 1;
  }
  return count;
}

static int location_distance(location_info a, location_info b)
{
    int dx = a.x - b.x;
    int dy = a.y - b.y;
    int dz = a.z - b.z;

    return (int) sqrt(dx*dx + dy*dy + dz*dz);
}

/***************************************************************************
 * CAMERA SMOOTHING
 ***************************************************************************/

#define CAMERA_SMOOTH_CURRENT      (CAMERA_SMOOTH_SIZE)
#define CAMERA_SMOOTH_HISTORY_SIZE (2 * CAMERA_SMOOTH_SIZE + 1)

static int get_next_angle(angle_info *a)
{
    block *b;

    for (b = a->b; b; b = b->next) {
        if (!block_is_timed(b)) {
            continue;
        }
        a->b = b;
        a->x = b->angles[0];
        a->y = b->angles[1];
        a->z = b->angles[2];
        return 1;
    }
    return 0;
}

static void smooth_camera_xy(demo *demo)
{
    int history_size = 0;
    angle_info angle;
    angle_info angle_future;
    angle_info history[CAMERA_SMOOTH_HISTORY_SIZE];

    block *b_start;
    int i;

    float x, y;
    double sum_cos, sum_sin;

    // populate some history
    b_start = demo->blocks;
 restart_angles:
    history_size = 0;
    while (b_start) {
        angle.b = b_start;
        if (!get_next_angle(&angle)) {
            goto finish_angles;
        }
        b_start = angle.b->next;
        history[history_size++] = angle;
        if (history_size == CAMERA_SMOOTH_HISTORY_SIZE) {
            break;
        }
    }

    while (1) {
        if (history_size < (CAMERA_SMOOTH_CURRENT + 1)) {
            b_start = history[history_size - 1].b->next;
            goto restart_angles;
        }

        // calculate average over the history frames
        x = 0;
        for (i = 0; i < history_size; i++) {
            x += history[i].x;
        }
        x /= history_size;

        sum_cos = 0; sum_sin = 0;
        for (i = 0; i < history_size; i++) {
            sum_cos += cos((history[i].y - 180) * M_PI / 180);
            sum_sin += sin((history[i].y - 180) * M_PI / 180);
        }
        y = atan2(sum_sin, sum_cos) * 180 / M_PI;
        y += 180;

        // use the average
        history[CAMERA_SMOOTH_CURRENT].b->angles[0] = x;
        history[CAMERA_SMOOTH_CURRENT].b->angles[1] = y;

        // update any following untimed blocks
        {
            block *bunt = history[CAMERA_SMOOTH_CURRENT].b->next;
            for (; bunt && !block_is_timed(bunt); bunt = bunt->next) {
                bunt->angles[0] = x;
                bunt->angles[1] = y;
            }
        }

        // update history
        for (i = 0; i < (history_size - 1); i++) {
            history[i] = history[i + 1];
        }
        angle_future.b = history[history_size - 1].b->next;
        if (get_next_angle(&angle_future)) {
            history[history_size - 1] = angle_future;
        }
        else {
            history_size--;
        }
    }

 finish_angles:
    printf("camera smoothed, x and y axes\n");
}

static void smooth_camera_z(demo *demo)
{
    int history_size = 0;
    angle_info angle;
    angle_info angle_future;
    angle_info history[CAMERA_SMOOTH_HISTORY_SIZE];

    block *b_start;
    int i;

    float z;

    // populate some history
    b_start = demo->blocks;
 restart_angles:
    history_size = 0;
    while (b_start) {
        angle.b = b_start;
        if (!get_next_angle(&angle)) {
            goto finish_angles;
        }
        b_start = angle.b->next;
        history[history_size++] = angle;
        if (history_size == CAMERA_SMOOTH_HISTORY_SIZE) {
            break;
        }
    }

    while (1) {
        if (history_size < (CAMERA_SMOOTH_CURRENT + 1)) {
            b_start = history[history_size - 1].b->next;
            goto restart_angles;
        }

        z = 0;
        for (i = 0; i < history_size; i++) {
            z += history[i].z;
        }
        z /= history_size;

        // use the average
        history[CAMERA_SMOOTH_CURRENT].b->angles[2] = z;

        // update any following untimed blocks
        {
            block *bunt = history[CAMERA_SMOOTH_CURRENT].b->next;
            for (; bunt && !block_is_timed(bunt); bunt = bunt->next) {
                bunt->angles[2] = z;
            }
        }

        // update history
        for (i = 0; i < (history_size - 1); i++) {
            history[i] = history[i + 1];
        }
        angle_future.b = history[history_size - 1].b->next;
        if (get_next_angle(&angle_future)) {
            history[history_size - 1] = angle_future;
        }
        else {
            history_size--;
        }
    }

 finish_angles:
    printf("camera smoothed, z axis\n");
}

/***************************************************************************
 * MOTION SMOOTHING
 ***************************************************************************/

#define MOTION_SMOOTH_CURRENT      (MOTION_SMOOTH_SIZE)
#define MOTION_SMOOTH_HISTORY_SIZE (2 * MOTION_SMOOTH_SIZE + 1)

static int get_next_location(uint16_t camera, location_info *loc,
                             location_info *prev)
{
    block *b;
    message *m;
    location_info next;

    for (b = loc->b; b; b = b->next) {
        for (m = b->messages; m; m = m->next) {
            if (m->type >= 0x80) {
                uint32_t mask;
                uint16_t entity;
                char *data = (char *)m->data;

                mask = m->type & 0x7F;
                if (mask & 0x1) {
                    mask |= (*data << 8);
                    data += 1;
                }

                // fitzquake protocol
                if (mask & 0x8000) {
                    mask |= (*data << 16);
                    data += 1;
                }
                if (mask & 0x800000) {
                    mask |= (*data << 24);
                    data += 1;
                }

                // read entity id
                if (mask & 0x4000) {
                    entity = *(uint16_t *)data;
                    data += 2;
                }
                else {
                    entity = *(uint8_t *)data;
                    data += 1;
                }

                if (entity != camera) {
                    continue;
                }

                if ((mask & 0x0E) != 0x0E) {
                    if (prev == NULL) {
                        // incomplete location info
                        printf("warning: insufficient location info (mask 0x%x)\n",
                               mask);
                    }
                }

                // each of these bits cost an additional 1 byte
                data += count_setbits(mask & 0x3C40);

                // read x
                if (mask & 0x0002) {
                    loc->x_ptr =  (int16_t *)data;
                    loc->x     = *(int16_t *)data;
                    data += 2;
                }
                else {
                    loc->x_ptr = NULL;
                    if (prev) {
                        loc->x = prev->x;
                    }
                    else {
                        loc->x = 0;
                    }
                }

                // skip x-angle
                data += count_setbits(mask & 0x0100);

                // read y
                if (mask & 0x0004) {
                    loc->y_ptr =  (int16_t *)data;
                    loc->y     = *(int16_t *)data;
                    data += 2;
                }
                else {
                    loc->y_ptr = NULL;
                    if (prev) {
                        loc->y = prev->y;
                    }
                    else {
                        loc->y = 0;
                    }
                }

                // skip y angle
                data += count_setbits(mask & 0x0010);

                // read z
                if (mask & 0x0008) {
                    loc->z_ptr =  (int16_t *)data;
                    loc->z     = *(int16_t *)data;
                }
                else {
                    loc->z_ptr = NULL;
                    if (prev) {
                        loc->z = prev->z;
                    }
                    else {
                        loc->z = 0;
                    }
                }

                loc->b = b;
                return 1;
            }
        }
    }
    return 0;
}

static void smooth_motion(demo *demo)
{
    block *b;
    block *b_start;
    uint16_t camera = 1;

    int history_size = 0;
    location_info location;
    location_info location_future;
    location_info history[MOTION_SMOOTH_HISTORY_SIZE];

    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int i;

    b_start = demo->blocks;

    // populate some history
 restart_motion:
    history_size = 0;
    while (b_start) {
        location.b = b_start;
        if (!get_next_location(camera, &location, NULL)) {
            goto finish_motion;
        }
        b_start = location.b->next;
        history[history_size++] = location;
        if (history_size == MOTION_SMOOTH_HISTORY_SIZE) {
            break;
        }
    }

    // apply smoothness where possible
    while (1) {
        if (history_size < (MOTION_SMOOTH_CURRENT + 1)) {
            b_start = history[history_size - 1].b->next;
            goto restart_motion;
        }

        // calculate average over the history frames
        x = 0; y = 0; z = 0;
        for (i = 0; i < history_size; i++) {
            x += history[i].x;
            y += history[i].y;
            z += history[i].z;
        }
        x /= history_size;
        y /= history_size;
        z /= history_size;

        // use the average
        if (history[MOTION_SMOOTH_CURRENT].x_ptr) {
            *(history[MOTION_SMOOTH_CURRENT].x_ptr) = (int16_t) x;
        }
        if (history[MOTION_SMOOTH_CURRENT].y_ptr) {
            *(history[MOTION_SMOOTH_CURRENT].y_ptr) = (int16_t) y;
        }
        if (history[MOTION_SMOOTH_CURRENT].z_ptr) {
            *(history[MOTION_SMOOTH_CURRENT].z_ptr) = (int16_t) z;
        }

        // update history
        for (i = 0; i < (history_size - 1); i++) {
            history[i] = history[i + 1];
        }
        location_future.b = history[history_size - 1].b->next;
        if (get_next_location(camera, &location_future, &history[history_size - 1]) &&
            location_distance(location_future, history[history_size - 1]) < MOTION_SMOOTH_RESTART_LIMIT) {
            history[history_size - 1] = location_future;
        }
        else {
            history_size--;
        }
    }

 finish_motion:
    printf("motion smoothed\n");
}

/***************************************************************************
 * CAMERA ROLL
 ***************************************************************************/

static void add_roll(demo *demo)
{
    block *b;
    float prev_y;
    float curr_y;
    float curr_dy;
    float z = 0.0;

    if (demo->blocks) {
        prev_y = demo->blocks->angles[1];
    }

    for (b = demo->blocks; b; b = b->next) {
        curr_y = b->angles[1];
        curr_dy = prev_y - curr_y;

        if (curr_dy > ROLL_TRIGGER_ANGLE) {
            z += ROLL_SPEED;

            if (z > ROLL_TARGET) {
                z = ROLL_TARGET;
            }
        }
        else if (curr_dy < -1 * ROLL_TRIGGER_ANGLE) {
            z -= ROLL_SPEED;

            if (z < -1 * ROLL_TARGET) {
                z = -1 * ROLL_TARGET;
            }
        }
        else {
            // go back towards 0
            if (z < -1.0 * ROLL_SPEED) {
                z += ROLL_SPEED;
            }
            else if (z > ROLL_SPEED) {
                z -= ROLL_SPEED;
            }
            else {
                z = 0;
            }
        }

        b->angles[2] = z;

        prev_y = curr_y;
    }

    printf("camera rolls added\n");
}
