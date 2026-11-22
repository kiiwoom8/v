// Export packaged artwork at 125% DPI in the same coordinates as the app.
#include "../secretary.c"
#include <stdio.h>

int main(int argc, char **argv)
{
    GdiplusStartupInput input = {1, NULL, FALSE, TRUE};
    FILE *file;
    DWORD *blank;
    size_t count;
    int extra, pose;
    BOOL ok;

    if (argc != 2) {
        fprintf(stderr, "usage: tailmon_pose_export <poses.raw>\n");
        return 2;
    }
    g_hinst = GetModuleHandleW(NULL);
    if (GdiplusStartup(&gdiplus, &input, NULL) != Ok ||
        !load_tailmon_sheet(L"TAILMON_POSES", &image, &image_stream)) return 1;
    for (extra = 0; extra < SECRETARY_EXTRA_SHEETS; ++extra)
        if (tailmon_sheets[extra] &&
            !load_tailmon_sheet(tailmon_sheets[extra], &extra_images[extra], &extra_streams[extra])) return 1;
    use_atlas = TRUE;
    dpi = 120;
    if (!draw_sprite()) return 1;
    count = (size_t)sprite.size.cx * sprite.size.cy;
    blank = calloc(count, sizeof(*blank));
    if (!blank) return 1;
    file = fopen(argv[1], "wb");
    if (!file) {
        free(blank);
        return 1;
    }
    ok = fwrite(&sprite.size, sizeof(sprite.size), 1, file) == 1;
    for (pose = 0; ok && pose < POSE_COUNT; ++pose) {
        int sheet = pose / POSES_PER_SHEET - 1;
        const DWORD *pixels = sheet < 0 || extra_ready[sheet] ? pose_pixels + pose * count : blank;
        ok = fwrite(pixels, sizeof(*pixels), count, file) == count;
    }
    if (fclose(file)) ok = FALSE;
    free(blank);
    return ok ? 0 : 1;
}
