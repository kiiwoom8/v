// labelbox.h - where an element's label box is drawn: at the element's
// top-left corner, inside the monitor, and (labels_apart) clear of the boxes
// already placed. Shared by hint.c (the overlay) and tools/hint_check.c (its
// replica, so the checker reports the boxes the overlay draws). Pure
// geometry in monitor-relative pixels: no GDI, no globals.
#ifndef LABELBOX_H
#define LABELBOX_H

#include <windows.h>

// How many placed boxes a new box moves past before it stays put.
#define LABEL_PLACE_TRIES 8
// Pixels of monitor left between two boxes kept apart.
#define LABEL_PLACE_GAP 1

static LONG64 labelbox_area(const RECT *r)
{
    LONG64 w = (LONG64)r->right - r->left;
    LONG64 h = (LONG64)r->bottom - r->top;

    return w > 0 && h > 0 ? w * h : 0;
}

// TRUE when b covers more than a quarter of a.
static BOOL labelbox_covered(const RECT *a, const RECT *b)
{
    RECT in;

    if (!IntersectRect(&in, a, b))
        return FALSE;
    return labelbox_area(&in) * 4 > labelbox_area(a);
}

// The box as drawn before anything else is considered: bw x bh pixels
// pinned at (left, top), the element's top-left corner, nudged inside the
// monitor of mon_w x mon_h.
static void labelbox_natural(RECT *box, int left, int top, int bw, int bh,
                             int mon_w, int mon_h)
{
    if (left + bw > mon_w) left = mon_w - bw;
    if (top + bh > mon_h) top = mon_h - bh;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (bw > mon_w) bw = mon_w;
    if (bh > mon_h) bh = mon_h;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    box->left = left;
    box->top = top;
    box->right = left + bw;
    box->bottom = top + bh;
}

// labels_apart: a box that would cover more than a quarter of a box already
// placed, or be covered that much by one, moves to the right of that box,
// or below it when the monitor's edge is in the way, and is checked again
// against every placed box; after LABEL_PLACE_TRIES moves, or with no room
// either way, it stays where it is. Callers place the smallest element
// first, so an element keeps its corner and the card around it moves
// (Google: a result's title link and the result block share one corner,
// and the block's label hid the link's).
static void labelbox_apart(RECT *box, const RECT *placed, int count,
                           int mon_w, int mon_h)
{
    int tries;

    for (tries = 0; tries < LABEL_PLACE_TRIES; ++tries) {
        int bw = box->right - box->left;
        int bh = box->bottom - box->top;
        const RECT *hit = NULL;
        int i;

        for (i = 0; i < count; ++i) {
            if (placed[i].right <= box->left || placed[i].left >= box->right ||
                placed[i].bottom <= box->top || placed[i].top >= box->bottom)
                continue;
            if (labelbox_covered(box, &placed[i]) ||
                labelbox_covered(&placed[i], box)) {
                hit = &placed[i];
                break;
            }
        }
        if (!hit)
            return;
        if (hit->right + LABEL_PLACE_GAP + bw <= mon_w) {
            box->left = hit->right + LABEL_PLACE_GAP;
            box->right = box->left + bw;
        } else if (hit->bottom + LABEL_PLACE_GAP + bh <= mon_h) {
            box->top = hit->bottom + LABEL_PLACE_GAP;
            box->bottom = box->top + bh;
        } else {
            return;
        }
    }
}

#endif
