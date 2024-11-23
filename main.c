#include <X11/Xutil.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

#include <GL/glew.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <string.h>
#include <sys/types.h>

#include "util.h"
#include "config.h"

#define MIN_GLX_MAJOR 1
#define MIN_GLX_MINOR 3

typedef struct {
    bool isEnabled;
    GLfloat shadowPercentage;
    GLfloat radius;
    GLfloat deltaRadius;
} Flashlight;

void expose(XEvent *);
void keypress(XEvent *);
void checkGlxVersion(Display *);

static Display *dpy = NULL;
static int screen = 0;
static XWindowAttributes wa;
static Window w;
static bool running = true;

static void (*handler[LASTEvent]) (XEvent *) = {
    [Expose] = expose,
    [KeyPress] = keypress,
};

GLuint
loadShader(const char *name, GLenum type)
{
    FILE *fp = fopen(name, "r");
    GLchar *shaderSrc = NULL;

    if (fp == NULL)
        die("Unable to open shader file at '%s':", name);

    size_t len;
    ssize_t bytes_read = getdelim(&shaderSrc, &len, '\0', fp);

    if (bytes_read < 0)
        die("Unable to read shader file at '%s'.", name);
    fclose(fp);

    GLuint shader;

    shader = glCreateShader(type);

    glShaderSource(shader, 1, (const GLchar **)&shaderSrc, NULL);
    glCompileShader(shader);

    int sucess = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &sucess);
    if (!sucess) {
        GLchar infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
        die("Error comiling shader:\n%s", infoLog);
    }

    return shader;
}

void 
checkGlxVersion(Display *dpy)
{
    int glx_major, glx_minor;
    // Check shit if necessary
    if (!glXQueryVersion(dpy, &glx_major, &glx_minor)
        || ((glx_minor == MIN_GLX_MAJOR) && (glx_minor < MIN_GLX_MINOR))
        || (MIN_GLX_MAJOR < 1))
        die("Invalid GLX version %d.%d. Requires GLX >= %d.%d", glx_major, glx_minor, MIN_GLX_MAJOR, MIN_GLX_MINOR);
}

// TODO: implement support for the MIT shared memory extension. (MIT-SHM)
XImage*
getScreenshot()
{
    return XGetImage(
        dpy, w,
        0, 0,
        wa.width,
        wa.height,
        AllPlanes,
        ZPixmap);
}

void
destroyScreenshot(XImage *screenshot)
{
    XDestroyImage(screenshot);
}

int
main(int argc, char *argv[])
{
    Config conf = loadConf("config.conf");

    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        printf("Cannot connect to the X display server\n");
        exit(EXIT_FAILURE);
    }
    checkGlxVersion(dpy);

    screen = DefaultScreen(dpy);

    GLint attrs[] = {
        GLX_RGBA,
        GLX_DEPTH_SIZE, 24,
        GLX_DOUBLEBUFFER,
        None
    };

    XVisualInfo *vi = glXChooseVisual(dpy, screen, attrs);
    if (vi == NULL)
        die("No appropriate visual found");

    XSetWindowAttributes swa;
    memset(&swa,0,sizeof(XSetWindowAttributes));

    swa.colormap = XCreateColormap(dpy, DefaultRootWindow(dpy), vi->visual, AllocNone);
    swa.event_mask = ButtonPressMask 
        | ButtonReleaseMask 
        | KeyPressMask 
        | KeyReleaseMask 
        | PointerMotionMask 
        | ExposureMask 
        | ClientMessage;

    if (!conf.windowed) {
        swa.override_redirect = 1;
        swa.save_under = 1;
    }

    XGetWindowAttributes(dpy, DefaultRootWindow(dpy), &wa);

     w = XCreateWindow(
        dpy, DefaultRootWindow(dpy), 
        0, 0, wa.width, wa.height, 0,
        vi->depth, InputOutput, vi->visual,
        CWColormap | CWEventMask | CWOverrideRedirect | CWSaveUnder, &swa
    );

    XMapWindow(dpy, w);

    char *wmName  = "coomer";
    char *wmClass = "coomer";
    XClassHint hints = {.res_name = wmName, .res_class = wmClass};

    XStoreName(dpy, w, wmName);
    XSetClassHint(dpy, w, &hints);

    Atom wmDeleteAtom = XInternAtom(dpy, "WM_DELETE_WINDOW", 0);
    XSetWMProtocols(dpy, w, &wmDeleteAtom, 1);

    GLXContext glc = glXCreateContext(dpy, vi, NULL, GL_TRUE);
    glXMakeCurrent(dpy, w, glc);

    if (GLEW_OK != glewInit())
        die("Couldnt initialize glew!");
    
    glViewport(0, 0, wa.width, wa.height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    // TODO: figure out what loadExtensions means
    XSelectInput(dpy, w, ExposureMask | KeyPressMask);

    int revertToParent;
    Window origin_win;

    XGetInputFocus(dpy, &origin_win, &revertToParent);

    // TODO: make shader source a configurable dir
    // GLchar shaderDir[] = "./shaders/";

    GLuint vertexShader;
    GLchar vertexSrc[] = "./shaders/vertex.glsl";

    GLuint fragmentShader;
    GLchar fragmentSrc[] = "./shaders/fragment.glsl";

    /* Load and compile shaders */
    vertexShader   = loadShader(vertexSrc, GL_VERTEX_SHADER);
    fragmentShader = loadShader(fragmentSrc, GL_FRAGMENT_SHADER);

    /* Link shaders and create a program */
    GLuint shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);
    glLinkProgram(shaderProgram);

    int link_success;
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &link_success);

    if(!link_success) {
        GLchar infoLog[512];
        glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
        die("Error whilst linking program:\n%s", infoLog);
    }

    int ww = wa.width;
    int hh = wa.height;

    GLuint vbo, vao;
    GLfloat vertices[] = {
      -0.5f, -0.5f, 0.0f, // left  
        0.5f, -0.5f, 0.0f, // right 
        0.0f,  0.5f, 0.0f  // top   
    };
    /*
    GLfloat vertices[] = {
        //x   y     z       UV coords
        ww,   0.0f, 0.0f,   1.0f, 1.0f, //Top right
        ww,   hh,   0.0f,   1.0f, 0.0f, //Bot right
        0.0f, 0.0f, 0.0f,   0.0f, 1.0f, //Bot left 
        0.0f, hh,   0.0f,   0.0f, 0.0f, //Top left 
    };
    */
    /* Indecies of the triangles. 
     * We want to fill a screen rect so we create two triangles:
     * 3_____0
     * |\    |
     * |  \  |
     * 2____\1
     *
     * Therefore we have two triangles, 0-1-3 and 1-2-3.
     */
    GLuint indecies[] = {0, 1, 3, 1, 2, 3};

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vbo);

    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    GLsizei stride = 3 * sizeof(GLfloat);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(0);

    glUseProgram(shaderProgram);

    XEvent e;
    while (running) {
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(shaderProgram);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glXSwapBuffers(dpy, w);
        glFinish();

        // HACK: setting this every time is probably inefficient. Is there a 
        // better way to maintain fullscreen input focus?
        if (!conf.windowed)
            XSetInputFocus(dpy, w, RevertToParent, CurrentTime);
        XNextEvent(dpy, &e);
        switch (e.type) {
        case KeyPress:
            handler[e.type](&e);
            break;
        default:
            break;
        }
    }

    XSetInputFocus(dpy, origin_win, RevertToParent, CurrentTime);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, glc);

    XCloseDisplay(dpy);
    return 0;
}


void 
keypress(XEvent *e) 
{
    unsigned int i;
    KeySym keysym;
    XKeyEvent *ev;

    printf("event: keypress\n");
    ev = &e->xkey;
    keysym = XkbKeycodeToKeysym(dpy, (KeyCode)ev->keycode, 0, e->xkey.state & ShiftMask ? 1 : 0);
    if (keysym == XK_q || keysym == XK_Escape) {
        running = false; 
    }
}

void
expose(XEvent *e)
{
}
