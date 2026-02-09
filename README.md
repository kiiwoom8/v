Build and run with mingw32-make

1. Ensure you have MinGW (mingw32) installed and `mingw32-make` on PATH.
2. From the workspace root (where `m.c` is), run:

```
mingw32-make
```

3. Run the produced executable:

```
mouse_control_windows.exe
```

4. Clean build artifacts:

```
mingw32-make clean
```

Notes:
- The program uses the Windows `gdi32` functions; the Makefile links with `-lgdi32`.
- If you prefer the output name `m.exe`, edit the `TARGET` in the `Makefile` accordingly.
