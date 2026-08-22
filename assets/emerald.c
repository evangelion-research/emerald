#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define WIDTH 80
#define HEIGHT 30
#define SIZE (WIDTH * HEIGHT)

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    int a, b, c;
} Face;

static Vec3 vertices[] = {
    { 0.0f,  1.6f,  0.0f},

    {-1.0f,  0.6f, -0.7f},
    { 1.0f,  0.6f, -0.7f},
    { 1.0f,  0.6f,  0.7f},
    {-1.0f,  0.6f,  0.7f},

    {-0.8f, -0.6f, -0.55f},
    { 0.8f, -0.6f, -0.55f},
    { 0.8f, -0.6f,  0.55f},
    {-0.8f, -0.6f,  0.55f},

    { 0.0f, -1.6f,  0.0f}
};

static Face faces[] = {
    {0, 1, 2},
    {0, 2, 3},
    {0, 3, 4},
    {0, 4, 1},

    {1, 5, 6},
    {1, 6, 2},

    {2, 6, 7},
    {2, 7, 3},

    {3, 7, 8},
    {3, 8, 4},

    {4, 8, 5},
    {4, 5, 1},

    {9, 6, 5},
    {9, 7, 6},
    {9, 8, 7},
    {9, 5, 8}
};

static char buffer[SIZE];
static float zbuffer[SIZE];

static Vec3 sub(Vec3 a, Vec3 b)
{
    return (Vec3){
        a.x - b.x,
        a.y - b.y,
        a.z - b.z
    };
}

static Vec3 cross(Vec3 a, Vec3 b)
{
    return (Vec3){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static float dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 normalize(Vec3 v)
{
    float len = sqrtf(dot(v, v));

    if (len == 0.0f)
        return v;

    return (Vec3){
        v.x / len,
        v.y / len,
        v.z / len
    };
}

static Vec3 rotate(Vec3 p, float A, float B, float C)
{
    float ca = cosf(A);
    float sa = sinf(A);

    float cb = cosf(B);
    float sb = sinf(B);

    float cc = cosf(C);
    float sc = sinf(C);

    /* rotate around X */
    {
        float y = p.y * ca - p.z * sa;
        float z = p.y * sa + p.z * ca;

        p.y = y;
        p.z = z;
    }

    /* rotate around Y */
    {
        float x = p.x * cb + p.z * sb;
        float z = -p.x * sb + p.z * cb;

        p.x = x;
        p.z = z;
    }

    /* rotate around Z */
    {
        float x = p.x * cc - p.y * sc;
        float y = p.x * sc + p.y * cc;

        p.x = x;
        p.y = y;
    }

    return p;
}

static void clear_screen_buffers(void)
{
    memset(buffer, ' ', sizeof(buffer));

    for (int i = 0; i < SIZE; i++)
        zbuffer[i] = 0.0f;
}

static void draw_sample(Vec3 p, float luminance)
{
    const char shades[] = ".,-~:;=!*#$@";
    const int shade_count = sizeof(shades) - 2;

    const float camera_distance = 5.0f;

    float z = p.z + camera_distance;

    if (z <= 0.1f)
        return;

    float inv_z = 1.0f / z;

    /*
        Terminal characters are taller than they are wide, so
        X gets stretched a bit to compensate.
    */
    int screen_x =
        WIDTH / 2 +
        (int)(45.0f * p.x * inv_z);

    int screen_y =
        HEIGHT / 2 -
        (int)(22.0f * p.y * inv_z);

    if (screen_x < 0 ||
        screen_x >= WIDTH ||
        screen_y < 0 ||
        screen_y >= HEIGHT)
        return;

    int index = screen_x + screen_y * WIDTH;

    if (inv_z <= zbuffer[index])
        return;

    zbuffer[index] = inv_z;

    if (luminance < 0.0f)
        luminance = 0.0f;

    if (luminance > 1.0f)
        luminance = 1.0f;

    int shade =
        (int)(luminance * shade_count);

    buffer[index] = shades[shade];
}

static void render_triangle(
    Vec3 a,
    Vec3 b,
    Vec3 c,
    float A,
    float B,
    float C
)
{
    /*
        Rotate triangle vertices.
    */
    Vec3 ra = rotate(a, A, B, C);
    Vec3 rb = rotate(b, A, B, C);
    Vec3 rc = rotate(c, A, B, C);

    /*
        Surface normal.
    */
    Vec3 edge1 = sub(rb, ra);
    Vec3 edge2 = sub(rc, ra);

    Vec3 normal = normalize(cross(edge1, edge2));

    /*
        Light coming from slightly above, left and toward viewer.
    */
    Vec3 light = normalize((Vec3){
        -0.5f,
         1.0f,
        -1.0f
    });

    float luminance = dot(normal, light);

    /*
        Back-facing / unlit surfaces aren't useful.
    */
    if (luminance <= 0.0f)
        return;

    /*
        Sample points across the triangle using barycentric coordinates.

        p = a + u(b-a) + v(c-a)

        with:
            u >= 0
            v >= 0
            u + v <= 1
    */
    const float step = 0.025f;

    for (float u = 0.0f; u <= 1.0f; u += step) {

        for (
            float v = 0.0f;
            v <= 1.0f - u;
            v += step
        ) {

            Vec3 p = {
                ra.x +
                    u * (rb.x - ra.x) +
                    v * (rc.x - ra.x),

                ra.y +
                    u * (rb.y - ra.y) +
                    v * (rc.y - ra.y),

                ra.z +
                    u * (rb.z - ra.z) +
                    v * (rc.z - ra.z)
            };

            /*
                Add a small variation across the facet.

                This gives the gem a little more visual texture
                while preserving the flat-facet lighting.
            */
            float edge_glow =
                0.15f * sinf(u * 15.0f + v * 10.0f);

            float brightness =
                luminance + edge_glow;

            if (brightness < 0.0f)
                brightness = 0.0f;

            if (brightness > 1.0f)
                brightness = 1.0f;

            draw_sample(p, brightness);
        }
    }
}

static void render(float A, float B, float C)
{
    int face_count =
        sizeof(faces) / sizeof(faces[0]);

    for (int i = 0; i < face_count; i++) {

        Face f = faces[i];

        render_triangle(
            vertices[f.a],
            vertices[f.b],
            vertices[f.c],
            A,
            B,
            C
        );
    }
}

static void print_frame(void)
{
    /*
        ANSI:
        \x1b[H = cursor home
        38;2;... = RGB foreground color

        This makes the ASCII emerald actually emerald green.
    */
    printf("\x1b[H");
    printf("\x1b[38;2;20;220;120m");

    for (int y = 0; y < HEIGHT; y++) {

        for (int x = 0; x < WIDTH; x++) {

            int index = x + y * WIDTH;

            putchar(buffer[index]);
        }

        putchar('\n');
    }

    printf("\x1b[0m");

    fflush(stdout);
}

int main(void)
{
    float A = 0.3f;
    float B = 0.0f;
    float C = 0.0f;

    /*
        Clear terminal.
    */
    printf("\x1b[2J");

    /*
        Hide cursor.
    */
    printf("\x1b[?25l");

    while (1) {

        clear_screen_buffers();

        render(A, B, C);

        print_frame();

        /*
            Similar idea to donut.c:
            continuously modify rotation angles.
        */
        A += 0.008f;
        B += 0.025f;
        C += 0.004f;

        /*
            ~60 FPS
        */
        usleep(16000);
    }

    /*
        Unreachable normally, but restores cursor if you
        change the loop later.
    */
    printf("\x1b[?25h");

    return 0;
}
