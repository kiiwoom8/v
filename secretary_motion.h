#ifndef SECRETARY_MOTION_H
#define SECRETARY_MOTION_H

typedef struct {
    unsigned first, count;
    BOOL reverse, flip;
} SecretaryMotion;

void secretary_motion_open(HINSTANCE instance, int width, int height);
void secretary_motion_close(void);
SecretaryMotion secretary_motion_find(int from, int to, int pose_count);
unsigned secretary_motion_routine_count(void);
SecretaryMotion secretary_motion_routine(unsigned number);
BOOL secretary_motion_paint(SecretaryMotion motion, unsigned frame, DWORD *pixels);

#endif
