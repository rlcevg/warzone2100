/*
	This file is part of Warzone 2100.
	Copyright (C) 2013  Warzone 2100 Project

	Warzone 2100 is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Warzone 2100 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Warzone 2100; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/
/**
 * @file main_sdl.cpp
 *
 * SDL backend code
 */

#include <QtGui/QApplication>
#include <QtCore/QString>
#include "lib/framework/wzapp.h"
#include "lib/framework/input.h"
#include "lib/framework/utf.h"
#include "lib/framework/opengl.h"
#include "lib/ivis_opengl/pieclip.h"
#include "lib/gamelib/gtime.h"
#include "src/warzoneconfig.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
#include "scrap.h"
#include "wz2100icon.h"
#include "cursors_sdl.h"
#include <algorithm>
#include <map>

extern void mainLoop();
// used in crash reports & version info
const char *BACKEND="SDL";
std::map<KEY_CODE, SDL_Keycode> KEY_CODE_to_SDLKey;
std::map<SDL_Keycode, KEY_CODE > SDLKey_to_KEY_CODE;

int realmain(int argc, char *argv[]);
int main(int argc, char *argv[])
{
	return realmain(argc, argv);
}

static SDL_Window *WZwindow = NULL;
static SDL_GLContext WZglcontext = NULL;
unsigned	screenWidth = 0;   // Declared in frameint.h.
unsigned	screenHeight = 0;  // Declared in frameint.h.

static std::vector<screeninfo> displaylist;	// holds all our possible display lists

QCoreApplication *appPtr;


/* The possible states for keys */
enum KEY_STATE
{
	KEY_UP,
	KEY_PRESSED,
	KEY_DOWN,
	KEY_RELEASED,
	KEY_PRESSRELEASE,	// When a key goes up and down in a frame
	KEY_DOUBLECLICK,	// Only used by mouse keys
	KEY_DRAG			// Only used by mouse keys
};

struct INPUT_STATE
{
	KEY_STATE state; /// Last key/mouse state
	UDWORD lastdown; /// last key/mouse button down timestamp
	Vector2i pressPos;    ///< Location of last mouse press event.
	Vector2i releasePos;  ///< Location of last mouse release event.
};

/// constant for the interval between 2 singleclicks for doubleclick event in ms
#define DOUBLE_CLICK_INTERVAL 250

/* The current state of the keyboard */
static INPUT_STATE aKeyState[KEY_MAXSCAN];		// NOTE: SDL_NUM_SCANCODES is the max, but KEY_MAXSCAN is our limit

/* The current location of the mouse */
static Uint16 mouseXPos = 0;
static Uint16 mouseYPos = 0;
static bool mouseInWindow = true;

/* How far the mouse has to move to start a drag */
#define DRAG_THRESHOLD	5

/* Which button is being used for a drag */
static MOUSE_KEY_CODE dragKey;

/* The start of a possible drag by the mouse */
static int dragX = 0;
static int dragY = 0;

/* The current mouse button state */
static INPUT_STATE aMouseState[MOUSE_BAD];
static MousePresses mousePresses;

/* The size of the input buffer */
#define INPUT_MAXSTR	512

/* The input string buffer */
struct InputKey
{
	UDWORD key;
	utf_32_char unicode;
};
static InputKey	pInputBuffer[INPUT_MAXSTR];
static InputKey	*pStartBuffer, *pEndBuffer;

/**************************/
/***     Misc support   ***/
/**************************/

/* Put a character into a text buffer overwriting any text under the cursor */
QString wzGetSelection()
{
	QString retval;
	static char* scrap = NULL;
	int scraplen;

	get_scrap(T('T','E','X','T'), &scraplen, &scrap);
	if (scraplen > 0)
	{
		retval = QString::fromUtf8(scrap);
	}
	return retval;
}


#if defined(WZ_WS_X11)

#include <GL/glx.h> // GLXDrawable
// X11 polution
#ifdef Status
#undef Status
#endif // Status
#ifdef CursorShape
#undef CursorShape
#endif // CursorShape
#ifdef Bool
#undef Bool
#endif // Bool

#ifndef GLX_SWAP_INTERVAL_EXT
#define GLX_SWAP_INTERVAL_EXT 0x20F1
#endif // GLX_SWAP_INTERVAL_EXT

// Need this global for use case of only having glXSwapIntervalSGI
static int swapInterval = -1;

void wzSetSwapInterval(int interval)
{
	typedef void (* PFNGLXQUERYDRAWABLEPROC) (Display *, GLXDrawable, int, unsigned int *);
	typedef void ( * PFNGLXSWAPINTERVALEXTPROC) (Display*, GLXDrawable, int);
	typedef int (* PFNGLXGETSWAPINTERVALMESAPROC)(void);
	typedef int (* PFNGLXSWAPINTERVALMESAPROC)(unsigned);
	typedef int (* PFNGLXSWAPINTERVALSGIPROC) (int);
	PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT;
	PFNGLXQUERYDRAWABLEPROC glXQueryDrawable;
	PFNGLXGETSWAPINTERVALMESAPROC glXGetSwapIntervalMESA;
	PFNGLXSWAPINTERVALMESAPROC glXSwapIntervalMESA;
	PFNGLXSWAPINTERVALSGIPROC glXSwapIntervalSGI;

	if (interval < 0)
			interval = 0;

#if GLX_VERSION_1_2
	// Hack-ish, but better than not supporting GLX_SWAP_INTERVAL_EXT?
	GLXDrawable drawable = glXGetCurrentDrawable();
	Display * display =  glXGetCurrentDisplay();
	glXSwapIntervalEXT = (PFNGLXSWAPINTERVALEXTPROC) SDL_GL_GetProcAddress("glXSwapIntervalEXT");
	glXQueryDrawable = (PFNGLXQUERYDRAWABLEPROC) SDL_GL_GetProcAddress("glXQueryDrawable");

	if (glXSwapIntervalEXT && glXQueryDrawable && drawable)
	{
		unsigned clampedInterval;
		glXSwapIntervalEXT(display, drawable, interval);
		glXQueryDrawable(display, drawable, GLX_SWAP_INTERVAL_EXT, &clampedInterval);
		swapInterval = clampedInterval;
		return;
	}
#endif

	glXSwapIntervalMESA = (PFNGLXSWAPINTERVALMESAPROC) SDL_GL_GetProcAddress("glXSwapIntervalMESA");
	glXGetSwapIntervalMESA = (PFNGLXGETSWAPINTERVALMESAPROC) SDL_GL_GetProcAddress("glXGetSwapIntervalMESA");
	if (glXSwapIntervalMESA && glXGetSwapIntervalMESA)
	{
		glXSwapIntervalMESA(interval);
		swapInterval = glXGetSwapIntervalMESA();
		return;
	}

	glXSwapIntervalSGI = (PFNGLXSWAPINTERVALSGIPROC) SDL_GL_GetProcAddress("glXSwapIntervalSGI");
	if (glXSwapIntervalSGI)
	{
		if (interval < 1)
			interval = 1;
		if (glXSwapIntervalSGI(interval))
		{
			// Error, revert to default
			swapInterval = 1;
			glXSwapIntervalSGI(1);
		}
		else
		{
			swapInterval = interval;
		}
		return;
	}
	swapInterval = 0;
}

int wzGetSwapInterval()
{
	if (swapInterval >= 0)
		return swapInterval;

	typedef void (* PFNGLXQUERYDRAWABLEPROC) (Display *, GLXDrawable, int, unsigned int *);
	typedef int (* PFNGLXGETSWAPINTERVALMESAPROC)(void);
	typedef int (* PFNGLXSWAPINTERVALSGIPROC) (int);
	PFNGLXQUERYDRAWABLEPROC glXQueryDrawable;
	PFNGLXGETSWAPINTERVALMESAPROC glXGetSwapIntervalMESA;
	PFNGLXSWAPINTERVALSGIPROC glXSwapIntervalSGI;

#if GLX_VERSION_1_2
	// Hack-ish, but better than not supporting GLX_SWAP_INTERVAL_EXT?
	GLXDrawable drawable = glXGetCurrentDrawable();
	Display * display =  glXGetCurrentDisplay();
	glXQueryDrawable = (PFNGLXQUERYDRAWABLEPROC) SDL_GL_GetProcAddress("glXQueryDrawable");

	if (glXQueryDrawable && drawable)
	{
		unsigned interval;
		glXQueryDrawable(display, drawable, GLX_SWAP_INTERVAL_EXT, &interval);
		swapInterval = interval;
		return swapInterval;
	}
#endif

	glXGetSwapIntervalMESA = (PFNGLXGETSWAPINTERVALMESAPROC) SDL_GL_GetProcAddress("glXGetSwapIntervalMESA");
	if (glXGetSwapIntervalMESA)
	{
		swapInterval = glXGetSwapIntervalMESA();
		return swapInterval;
	}

	glXSwapIntervalSGI = (PFNGLXSWAPINTERVALSGIPROC) SDL_GL_GetProcAddress("glXSwapIntervalSGI");
	if (glXSwapIntervalSGI)
	{
		swapInterval = 1;
	}
	else
	{
		swapInterval = 0;
	}
	return swapInterval;
}

#elif defined(WZ_WS_WIN)

void wzSetSwapInterval(int interval)
{
	typedef BOOL (WINAPI * PFNWGLSWAPINTERVALEXTPROC) (int);
	PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT;

	if (interval < 0)
		interval = 0;

	wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC) SDL_GL_GetProcAddress("wglSwapIntervalEXT");

	if (wglSwapIntervalEXT)
		wglSwapIntervalEXT(interval);
}

int wzGetSwapInterval()
{
	typedef int (WINAPI * PFNWGLGETSWAPINTERVALEXTPROC) (void);
	PFNWGLGETSWAPINTERVALEXTPROC wglGetSwapIntervalEXT;

	wglGetSwapIntervalEXT = (PFNWGLGETSWAPINTERVALEXTPROC) SDL_GL_GetProcAddress("wglGetSwapIntervalEXT");
	if (wglGetSwapIntervalEXT)
		return wglGetSwapIntervalEXT();
	return 0;
}

#elif !defined(WZ_OS_MAC)

void wzSetSwapInterval(int)
{
	return;
}

int wzGetSwapInterval()
{
	return 0;
}

#endif

std::vector<screeninfo> wzAvailableResolutions()
{
	return displaylist;
}

void wzShowMouse(bool visible)
{
	SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
}

int wzGetTicks()
{
	return SDL_GetTicks();
}

void wzFatalDialog(const char *msg)
{
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                         "We have a problem!",
                         msg,
                         NULL);
}

void wzScreenFlip()
{
	SDL_GL_SwapWindow(WZwindow);
}

void wzToggleFullscreen()
{
	war_setFullscreen(!war_getFullscreen());

	Uint32 flags = SDL_GetWindowFlags(WZwindow);
	if (flags & SDL_WINDOW_FULLSCREEN)
	{
		flags &= ~SDL_WINDOW_FULLSCREEN;
	}
	else
	{
		flags |= SDL_WINDOW_FULLSCREEN;
	}

	SDL_SetWindowFullscreen(WZwindow, flags);
}

void wzQuit()
{
	// Create a quit event to halt game loop.
	SDL_Event quitEvent;
	quitEvent.type = SDL_QUIT;
	SDL_PushEvent(&quitEvent);
}

void wzGrabMouse()
{
	SDL_SetWindowGrab(WZwindow, SDL_TRUE);
}

void wzReleaseMouse()
{
	SDL_SetWindowGrab(WZwindow, SDL_FALSE);
}

/**************************/
/***    Thread support  ***/
/**************************/

WZ_THREAD *wzThreadCreate(int (*threadFunc)(void *), void *data)
{
	return (WZ_THREAD *)SDL_CreateThread(threadFunc, "wzThread", data);
}

int wzThreadJoin(WZ_THREAD *thread)
{
	int result;
	SDL_WaitThread((SDL_Thread *)thread, &result);
	return result;
}

void wzThreadStart(WZ_THREAD *thread)
{
	(void)thread; // no-op
}

void wzYieldCurrentThread()
{
	SDL_Delay(40);
}

WZ_MUTEX *wzMutexCreate()
{
	return (WZ_MUTEX *)SDL_CreateMutex();
}

void wzMutexDestroy(WZ_MUTEX *mutex)
{
	SDL_DestroyMutex((SDL_mutex *)mutex);
}

void wzMutexLock(WZ_MUTEX *mutex)
{
	SDL_LockMutex((SDL_mutex *)mutex);
}

void wzMutexUnlock(WZ_MUTEX *mutex)
{
	SDL_UnlockMutex((SDL_mutex *)mutex);
}

WZ_SEMAPHORE *wzSemaphoreCreate(int startValue)
{
	return (WZ_SEMAPHORE *)SDL_CreateSemaphore(startValue);
}

void wzSemaphoreDestroy(WZ_SEMAPHORE *semaphore)
{
	SDL_DestroySemaphore((SDL_sem *)semaphore);
}

void wzSemaphoreWait(WZ_SEMAPHORE *semaphore)
{
	SDL_SemWait((SDL_sem *)semaphore);
}

void wzSemaphorePost(WZ_SEMAPHORE *semaphore)
{
	SDL_SemPost((SDL_sem *)semaphore);
}

/**************************/
/***     Main support   ***/
/**************************/

static inline void initKeycodes()
{
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_ESC, SDLK_ESCAPE));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_1, SDLK_1));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_2, SDLK_2));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_3, SDLK_3));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_4, SDLK_4));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_5, SDLK_5));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_6, SDLK_6));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_7, SDLK_7));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_8, SDLK_8));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_9, SDLK_9));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_0, SDLK_0));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_MINUS, SDLK_MINUS));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_EQUALS, SDLK_EQUALS));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_BACKSPACE, SDLK_BACKSPACE));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_TAB, SDLK_TAB));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_Q, SDLK_q));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_W, SDLK_w));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_E, SDLK_e));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_R, SDLK_r));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_T, SDLK_t));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_Y, SDLK_y));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_U, SDLK_u));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_I, SDLK_i));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_O, SDLK_o));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_P, SDLK_p));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_LBRACE, SDLK_LEFTBRACKET));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_RBRACE, SDLK_RIGHTBRACKET));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_RETURN, SDLK_RETURN));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_LCTRL, SDLK_LCTRL));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_A, SDLK_a));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_S, SDLK_s));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_D, SDLK_d));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F, SDLK_f));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_G, SDLK_g));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_H, SDLK_h));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_J, SDLK_j));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_K, SDLK_k));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_L, SDLK_l));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_SEMICOLON, SDLK_SEMICOLON));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_QUOTE, SDLK_QUOTE));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_BACKQUOTE, SDLK_BACKQUOTE));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_LSHIFT, SDLK_LSHIFT));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_LMETA, SDLK_LGUI));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_LSUPER, SDLK_LGUI));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_BACKSLASH, SDLK_BACKSLASH));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_Z, SDLK_z));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_X, SDLK_x));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_C, SDLK_c));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_V, SDLK_v));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_B, SDLK_b));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_N, SDLK_n));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_M, SDLK_m));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_COMMA, SDLK_COMMA));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_FULLSTOP, SDLK_PERIOD));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_FORWARDSLASH, SDLK_SLASH));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_RSHIFT, SDLK_RSHIFT));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_RMETA, SDLK_RGUI));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_RSUPER, SDLK_RGUI));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_STAR, SDLK_KP_MULTIPLY));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_LALT, SDLK_LALT));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_SPACE, SDLK_SPACE));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_CAPSLOCK, SDLK_CAPSLOCK));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F1, SDLK_F1));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F2, SDLK_F2));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F3, SDLK_F3));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F4, SDLK_F4));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F5, SDLK_F5));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F6, SDLK_F6));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F7, SDLK_F7));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F8, SDLK_F8));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F9, SDLK_F9));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F10, SDLK_F10));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_NUMLOCK, SDLK_NUMLOCKCLEAR));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_SCROLLLOCK, SDLK_SCROLLLOCK));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_7, SDLK_KP_7));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_8, SDLK_KP_8));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_9, SDLK_KP_9));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_MINUS, SDLK_KP_MINUS));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_4, SDLK_KP_4));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_5, SDLK_KP_5));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_6, SDLK_KP_6));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_PLUS, SDLK_KP_PLUS));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_1, SDLK_KP_1));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_2, SDLK_KP_2));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_3, SDLK_KP_3));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_0, SDLK_KP_0));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_FULLSTOP, SDLK_KP_PERIOD));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F11, SDLK_F11));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_F12, SDLK_F12));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_RCTRL, SDLK_RCTRL));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KP_BACKSLASH, SDLK_KP_DIVIDE));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_RALT, SDLK_RALT));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_HOME, SDLK_HOME));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_UPARROW, SDLK_UP));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_PAGEUP, SDLK_PAGEUP));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_LEFTARROW, SDLK_LEFT));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_RIGHTARROW, SDLK_RIGHT));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_END, SDLK_END));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_DOWNARROW, SDLK_DOWN));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_PAGEDOWN, SDLK_PAGEDOWN));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_INSERT, SDLK_INSERT));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_DELETE, SDLK_DELETE));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_KPENTER, SDLK_KP_ENTER));
	KEY_CODE_to_SDLKey.insert(std::pair<KEY_CODE, SDL_Keycode>(KEY_IGNORE, SDL_Keycode(5190)));

	std::map<KEY_CODE, SDL_Keycode>::iterator it;
	for (it = KEY_CODE_to_SDLKey.begin(); it != KEY_CODE_to_SDLKey.end(); it++)
	{
		SDLKey_to_KEY_CODE.insert(std::pair<SDL_Keycode, KEY_CODE>(it->second,it->first));
	}
}

static inline KEY_CODE sdlKeyToKeyCode(SDL_Keycode key)
{
	std::map<SDL_Keycode, KEY_CODE >::iterator it;

	it = SDLKey_to_KEY_CODE.find(key);

	if (it != SDLKey_to_KEY_CODE.end())
	{
		return it->second;
	}

	return (KEY_CODE)key;
}

static inline SDL_Keycode keyCodeToSDLKey(KEY_CODE code)
{
	std::map<KEY_CODE, SDL_Keycode>::iterator it;

	it = KEY_CODE_to_SDLKey.find(code);

	if (it != KEY_CODE_to_SDLKey.end())
	{
		return it->second;
	}

	return (SDL_Keycode)code;
}

// Cyclic increment.
static InputKey *inputPointerNext(InputKey *p)
{
    return p + 1 == pInputBuffer + INPUT_MAXSTR ? pInputBuffer : p + 1;
}

/* add count copies of the characater code to the input buffer */
static void inputAddBuffer(UDWORD key, utf_32_char unicode)
{
	/* Calculate what pEndBuffer will be set to next */
	InputKey	*pNext = inputPointerNext(pEndBuffer);

	if (pNext == pStartBuffer)
	{
		return;  // Buffer full.
	}

	// Add key to buffer.
	pEndBuffer->key = key;
	pEndBuffer->unicode = unicode;
	pEndBuffer = pNext;
}


void keyScanToString(KEY_CODE code, char *ascii, UDWORD maxStringSize)
{
	if (code == KEY_LCTRL)
	{
		// shortcuts with modifier keys work with either key.
		strcpy(ascii, "Ctrl");
		return;
	}
	else if (code == KEY_LSHIFT)
	{
		// shortcuts with modifier keys work with either key.
		strcpy(ascii, "Shift");
		return;
	}
	else if (code == KEY_LALT)
	{
		// shortcuts with modifier keys work with either key.
		strcpy(ascii, "Alt");
		return;
	}
	else if (code == KEY_LMETA)
	{
		// shortcuts with modifier keys work with either key.
#ifdef WZ_OS_MAC
		strcpy(ascii, "Cmd");
#else
		strcpy(ascii, "Meta");
#endif
		return;
	}

	if (code < KEY_MAXSCAN)
	{
		snprintf(ascii, maxStringSize, "%s", SDL_GetKeyName(keyCodeToSDLKey(code)));
		if (ascii[0] >= 'a' && ascii[0] <= 'z' && ascii[1] != 0)
		{
			// capitalize
			ascii[0] += 'A'-'a';
			return;
		}
	}
	else
	{
		strcpy(ascii,"???");
	}
}


/* Initialise the input module */
void inputInitialise(void)
{
	unsigned int i;

	for (i = 0; i < KEY_MAXSCAN; i++)
	{
		aKeyState[i].state = KEY_UP;
	}

	for (i = 0; i < MOUSE_BAD; i++)
	{
		aMouseState[i].state = KEY_UP;
	}

	pStartBuffer = pInputBuffer;
	pEndBuffer = pInputBuffer;

	dragX = mouseXPos = screenWidth/2;
	dragY = mouseYPos = screenHeight/2;
	dragKey = MOUSE_LMB;

}

/* Clear the input buffer */
void inputClearBuffer(void)
{
	pStartBuffer = pInputBuffer;
	pEndBuffer = pInputBuffer;
}

/* Return the next key press or 0 if no key in the buffer.
 * The key returned will have been remaped to the correct ascii code for the
 * windows key map.
 * All key presses are buffered up (including windows auto repeat).
 */
UDWORD inputGetKey(utf_32_char *unicode)
{
	UDWORD	retVal;

	if (pStartBuffer == pEndBuffer)
	{
	    return 0;  // Buffer empty.
	}

	retVal = pStartBuffer->key;
	if (unicode)
	{
	    *unicode = pStartBuffer->unicode;
	}
	if (!retVal)
	{
	    retVal = ' ';  // Don't return 0 if we got a virtual key, since that's interpreted as no input.
	}
	pStartBuffer = inputPointerNext(pStartBuffer);

	return retVal;
}

MousePresses const &inputGetClicks()
{
	return mousePresses;
}


/*!
 * This is called once a frame so that the system can tell
 * whether a key was pressed this turn or held down from the last frame.
 */
void inputNewFrame(void)
{
	unsigned int i;

	/* Do the keyboard */
	for (i = 0; i < KEY_MAXSCAN; i++)
	{
		if (aKeyState[i].state == KEY_PRESSED)
		{
			aKeyState[i].state = KEY_DOWN;
		}
		else if ( aKeyState[i].state == KEY_RELEASED  ||
			  aKeyState[i].state == KEY_PRESSRELEASE )
		{
			aKeyState[i].state = KEY_UP;
		}
	}

	/* Do the mouse */
	for (i = 0; i < 6; i++)
	{
		if (aMouseState[i].state == KEY_PRESSED)
		{
			aMouseState[i].state = KEY_DOWN;
		}
		else if ( aMouseState[i].state == KEY_RELEASED
		       || aMouseState[i].state == KEY_DOUBLECLICK
		       || aMouseState[i].state == KEY_PRESSRELEASE )
		{
			aMouseState[i].state = KEY_UP;
		}
	}

	mousePresses.clear();
}

/*!
 * Release all keys (and buttons) when we lose focus
 */
// FIXME This seems to be totally ignored! (Try switching focus while the dragbox is open)
void inputLoseFocus(void)
{
	unsigned int i;

	/* Lost the window focus, have to take this as a global key up */
	for(i = 0; i < KEY_MAXSCAN; i++)
	{
		aKeyState[i].state = KEY_UP;
	}
	for (i = 0; i < 6; i++)
	{
		aMouseState[i].state = KEY_UP;
	}
}

/* This returns true if the key is currently depressed */
bool keyDown(KEY_CODE code)
{
	ASSERT_OR_RETURN(false, code < KEY_MAXSCAN, "Invalid keycode of %d!", (int)code);
	return (aKeyState[code].state != KEY_UP);
}

/* This returns true if the key went from being up to being down this frame */
bool keyPressed(KEY_CODE code)
{
	ASSERT_OR_RETURN(false, code < KEY_MAXSCAN, "Invalid keycode of %d!", (int)code);
	return ((aKeyState[code].state == KEY_PRESSED) || (aKeyState[code].state == KEY_PRESSRELEASE));
}

/* This returns true if the key went from being down to being up this frame */
bool keyReleased(KEY_CODE code)
{
	ASSERT_OR_RETURN(false, code < KEY_MAXSCAN, "Invalid keycode of %d!", (int)code);
	return ((aKeyState[code].state == KEY_RELEASED) || (aKeyState[code].state == KEY_PRESSRELEASE));
}

/* Return the X coordinate of the mouse */
Uint16 mouseX(void)
{
	return mouseXPos;
}

/* Return the Y coordinate of the mouse */
Uint16 mouseY(void)
{
	return mouseYPos;
}

bool wzMouseInWindow()
{
	return mouseInWindow;
}

Vector2i mousePressPos_DEPRECATED(MOUSE_KEY_CODE code)
{
	return aMouseState[code].pressPos;
}

Vector2i mouseReleasePos_DEPRECATED(MOUSE_KEY_CODE code)
{
	return aMouseState[code].releasePos;
}

/* This returns true if the mouse key is currently depressed */
bool mouseDown(MOUSE_KEY_CODE code)
{
	return (aMouseState[code].state != KEY_UP) ||

	       // holding down LMB and RMB counts as holding down MMB
	       (code == MOUSE_MMB && aMouseState[MOUSE_LMB].state != KEY_UP && aMouseState[MOUSE_RMB].state != KEY_UP);
}

/* This returns true if the mouse key was double clicked */
bool mouseDClicked(MOUSE_KEY_CODE code)
{
	return (aMouseState[code].state == KEY_DOUBLECLICK);
}

/* This returns true if the mouse key went from being up to being down this frame */
bool mousePressed(MOUSE_KEY_CODE code)
{
	return ((aMouseState[code].state == KEY_PRESSED) ||
			(aMouseState[code].state == KEY_DOUBLECLICK) ||
			(aMouseState[code].state == KEY_PRESSRELEASE));
}

/* This returns true if the mouse key went from being down to being up this frame */
bool mouseReleased(MOUSE_KEY_CODE code)
{
	return ((aMouseState[code].state == KEY_RELEASED) ||
			(aMouseState[code].state == KEY_DOUBLECLICK) ||
			(aMouseState[code].state == KEY_PRESSRELEASE));
}

/* Check for a mouse drag, return the drag start coords if dragging */
bool mouseDrag(MOUSE_KEY_CODE code, UDWORD *px, UDWORD *py)
{
	if ((aMouseState[code].state == KEY_DRAG) ||

	    // dragging LMB and RMB counts as dragging MMB
	    (code == MOUSE_MMB && ((aMouseState[MOUSE_LMB].state == KEY_DRAG && aMouseState[MOUSE_RMB].state != KEY_UP) ||
	     (aMouseState[MOUSE_LMB].state != KEY_UP && aMouseState[MOUSE_RMB].state == KEY_DRAG))))
	{
		*px = dragX;
		*py = dragY;
		return true;
	}

	return false;
}

// TODO: Rewrite this silly thing
// FIXME: Is this even useful for anything but the sliders (that have been removed?)
void setMousePos(uint16_t x, uint16_t y)
{
	static int mousewarp = -1;

	if (mousewarp == -1)
	{
		int val;

		mousewarp = 1;
		if ((val = getMouseWarp()))
		{
			mousewarp = !val;
		}
	}
	if (mousewarp)
		SDL_WarpMouseInWindow(WZwindow, x, y);	// NOTE: need to fix this if we use multiple windows
}

/*!
 * Handle keyboard events
 */
static void inputHandleKeyEvent(SDL_KeyboardEvent * keyEvent)
{
	UDWORD code, vk;
	utf_32_char unicode = sdlKeyToKeyCode(keyEvent->keysym.sym);
	switch (keyEvent->type)
	{
		case SDL_KEYDOWN:
			switch (keyEvent->keysym.sym)
			{
				case SDLK_LEFT:
					vk = INPBUF_LEFT;
					break;
				case SDLK_RIGHT:
					vk = INPBUF_RIGHT;
					break;
				case SDLK_UP:
					vk = INPBUF_UP;
					break;
				case SDLK_DOWN:
					vk = INPBUF_DOWN;
					break;
				case SDLK_HOME:
					vk = INPBUF_HOME;
					break;
				case SDLK_END:
					vk = INPBUF_END;
					break;
				case SDLK_INSERT:
					vk = INPBUF_INS;
					break;
				case SDLK_DELETE:
					vk = INPBUF_DEL;
					break;
				case SDLK_PAGEUP:
					vk = INPBUF_PGUP;
					break;
				case SDLK_PAGEDOWN:
					vk = INPBUF_PGDN;
					break;
				default:
					vk = keyEvent->keysym.sym;
					break;
			}

			debug( LOG_INPUT, "Key Code (pressed): 0x%x, %d, [%c] SDLkey=[%s]", vk, vk, (vk < 128) && (vk > 31) ? (char) vk : '?' , SDL_GetKeyName(sdlKeyToKeyCode(keyEvent->keysym.sym)));//keyCodeToSDLKey((KEY_CODE)keyEvent->keysym.sym)));
			if (unicode < 32)
			{
				unicode =0;
			}
			inputAddBuffer(vk, unicode);	//FIXME there is no
			code = sdlKeyToKeyCode(keyEvent->keysym.sym);
			if ( aKeyState[code].state == KEY_UP ||
				 aKeyState[code].state == KEY_RELEASED ||
				 aKeyState[code].state == KEY_PRESSRELEASE )
			{
				//whether double key press or not
					aKeyState[code].state = KEY_PRESSED;
					aKeyState[code].lastdown = 0;
			}
			break;
		case SDL_KEYUP:
			code = sdlKeyToKeyCode(keyEvent->keysym.sym);
			if (aKeyState[code].state == KEY_PRESSED)
			{
				aKeyState[code].state = KEY_PRESSRELEASE;
			}
			else if (aKeyState[code].state == KEY_DOWN )
			{
				aKeyState[code].state = KEY_RELEASED;
			}
			break;
		default:
			break;
	}
}
/*!
 * Handle text events
*/

void inputhandleText(SDL_TextInputEvent *Tevent)
{
	// Since SDL now supports a Text input even, in theory, we turn this on when we need input from the user via
	// SDL_StartTextInput() and get the finished unicode string when SDL_StopTextInput() is processed.
	// This function would then get the Tevent->text and we would do whatever with it.
}
/*!
 * Handle mousebutton events
 */
static void inputHandleMouseWheelEvent(SDL_MouseWheelEvent *wheel)
{
	if (wheel->x >0 || wheel->y >0)
	{
		aMouseState[MOUSE_WUP].state = KEY_PRESSED;
		aMouseState[MOUSE_WUP].lastdown = 0;
	}
	else if (wheel->x <0 || wheel->y <0)
	{
		aMouseState[MOUSE_WDN].state = KEY_PRESSED;
		aMouseState[MOUSE_WDN].lastdown = 0;
	}
}
/*!
 * Handle mousebutton events
 */
static void inputHandleMouseButtonEvent(SDL_MouseButtonEvent * buttonEvent)
{
	mouseXPos = buttonEvent->x;
	mouseYPos = buttonEvent->y;

	MOUSE_KEY_CODE mouseKeyCode;
	switch (buttonEvent->button)
	{
		default: return;  // Unknown button.
		case SDL_BUTTON_LEFT: mouseKeyCode = MOUSE_LMB; break;
		case SDL_BUTTON_MIDDLE: mouseKeyCode = MOUSE_MMB; break;
		case SDL_BUTTON_RIGHT: mouseKeyCode = MOUSE_RMB; break;
		case SDL_BUTTON_X1: mouseKeyCode = MOUSE_X1; break;
		case SDL_BUTTON_X2: mouseKeyCode = MOUSE_X2; break;
	}

	MousePress mousePress;
	mousePress.key = mouseKeyCode;
	mousePress.pos = Vector2i(mouseXPos, mouseYPos);

	switch (buttonEvent->type)
	{
		case SDL_MOUSEBUTTONDOWN:
			mousePress.action = MousePress::Press;
			mousePresses.push_back(mousePress);

			aMouseState[mouseKeyCode].pressPos.x = mouseXPos;
			aMouseState[mouseKeyCode].pressPos.y = mouseYPos;
			if ( aMouseState[mouseKeyCode].state == KEY_UP
				|| aMouseState[mouseKeyCode].state == KEY_RELEASED
				|| aMouseState[mouseKeyCode].state == KEY_PRESSRELEASE )
			{
				// whether double click or not
				if ( realTime - aMouseState[mouseKeyCode].lastdown < DOUBLE_CLICK_INTERVAL )
				{
					aMouseState[mouseKeyCode].state = KEY_DOUBLECLICK;
					aMouseState[mouseKeyCode].lastdown = 0;
				}
				else
				{
					aMouseState[mouseKeyCode].state = KEY_PRESSED;
					aMouseState[mouseKeyCode].lastdown = realTime;
				}

				if (mouseKeyCode < 4) // Assume they are draggin' with either LMB|RMB|MMB
				{
					dragKey = mouseKeyCode;
					dragX = mouseXPos;
					dragY = mouseYPos;
				}
			}
			break;
		case SDL_MOUSEBUTTONUP:
			mousePress.action = MousePress::Release;
			mousePresses.push_back(mousePress);

			aMouseState[mouseKeyCode].releasePos.x = mouseXPos;
			aMouseState[mouseKeyCode].releasePos.y = mouseYPos;
			if (aMouseState[mouseKeyCode].state == KEY_PRESSED)
			{
				aMouseState[mouseKeyCode].state = KEY_PRESSRELEASE;
			}
			else if ( aMouseState[mouseKeyCode].state == KEY_DOWN
					|| aMouseState[mouseKeyCode].state == KEY_DRAG
					|| aMouseState[mouseKeyCode].state == KEY_DOUBLECLICK)
			{
				aMouseState[mouseKeyCode].state = KEY_RELEASED;
			}
			break;
		default:
			break;
	}
}


/*!
 * Handle mousemotion events
 */
static void inputHandleMouseMotionEvent(SDL_MouseMotionEvent * motionEvent)
{
	switch (motionEvent->type)
	{
		case SDL_MOUSEMOTION:
			/* store the current mouse position */
			mouseXPos = motionEvent->x;
			mouseYPos = motionEvent->y;

			/* now see if a drag has started */
			if ((aMouseState[dragKey].state == KEY_PRESSED ||
			     aMouseState[dragKey].state == KEY_DOWN) &&
			    (ABSDIF(dragX, mouseXPos) > DRAG_THRESHOLD ||
			     ABSDIF(dragY, mouseYPos) > DRAG_THRESHOLD))
			{
				aMouseState[dragKey].state = KEY_DRAG;
			}
			break;
		default:
			break;
	}
}

// Get human readable results
const char *getSDL_fmt_string(Uint32 format)
{
switch (format)
	{
	case SDL_PIXELFORMAT_INDEX1LSB: return "SDL_PIXELFORMAT_INDEX1LSB"; break;
	case SDL_PIXELFORMAT_INDEX1MSB: return "SDL_PIXELFORMAT_INDEX1MSB"; break;
	case SDL_PIXELFORMAT_INDEX4LSB: return "SDL_PIXELFORMAT_INDEX4LSB"; break;
	case SDL_PIXELFORMAT_INDEX4MSB: return "SDL_PIXELFORMAT_INDEX4MSB"; break;
	case SDL_PIXELFORMAT_INDEX8: return "SDL_PIXELFORMAT_INDEX8"; break;
	case SDL_PIXELFORMAT_RGB332: return "SDL_PIXELFORMAT_RGB332"; break;
	case SDL_PIXELFORMAT_RGB444: return "SDL_PIXELFORMAT_RGB444"; break;
	case SDL_PIXELFORMAT_RGB555: return "SDL_PIXELFORMAT_RGB555"; break;
	case SDL_PIXELFORMAT_BGR555: return "SDL_PIXELFORMAT_BGR555"; break;
	case SDL_PIXELFORMAT_ARGB4444: return "SDL_PIXELFORMAT_ARGB4444"; break;
	case SDL_PIXELFORMAT_RGBA4444: return "SDL_PIXELFORMAT_RGBA4444"; break;
	case SDL_PIXELFORMAT_ABGR4444: return "SDL_PIXELFORMAT_ABGR4444"; break;
	case SDL_PIXELFORMAT_BGRA4444: return "SDL_PIXELFORMAT_BGRA4444"; break;
	case SDL_PIXELFORMAT_ARGB1555: return "SDL_PIXELFORMAT_ARGB1555"; break;
	case SDL_PIXELFORMAT_RGBA5551: return "SDL_PIXELFORMAT_RGBA5551"; break;
	case SDL_PIXELFORMAT_ABGR1555: return "SDL_PIXELFORMAT_ABGR1555"; break;
	case SDL_PIXELFORMAT_BGRA5551: return "SDL_PIXELFORMAT_BGRA5551"; break;
	case SDL_PIXELFORMAT_RGB565: return "SDL_PIXELFORMAT_RGB565"; break;
	case SDL_PIXELFORMAT_BGR565: return "SDL_PIXELFORMAT_BGR565"; break;
	case SDL_PIXELFORMAT_RGB24: return "SDL_PIXELFORMAT_RGB24"; break;
	case SDL_PIXELFORMAT_BGR24: return "SDL_PIXELFORMAT_BGR24"; break;
	case SDL_PIXELFORMAT_RGB888: return "SDL_PIXELFORMAT_RGB888"; break;
	case SDL_PIXELFORMAT_RGBX8888: return "SDL_PIXELFORMAT_RGBX8888"; break;
	case SDL_PIXELFORMAT_BGR888: return "SDL_PIXELFORMAT_BGR888"; break;
	case SDL_PIXELFORMAT_BGRX8888: return "SDL_PIXELFORMAT_BGRX8888"; break;
	case SDL_PIXELFORMAT_ARGB8888: return "SDL_PIXELFORMAT_ARGB8888"; break;
	case SDL_PIXELFORMAT_RGBA8888: return "SDL_PIXELFORMAT_RGBA8888"; break;
	case SDL_PIXELFORMAT_ABGR8888: return "SDL_PIXELFORMAT_ABGR8888"; break;
	case SDL_PIXELFORMAT_BGRA8888: return "SDL_PIXELFORMAT_BGRA8888"; break;
	case SDL_PIXELFORMAT_ARGB2101010: return "SDL_PIXELFORMAT_ARGB2101010"; break;
	case SDL_PIXELFORMAT_YV12: return "SDL_PIXELFORMAT_YV12"; break;
	case SDL_PIXELFORMAT_IYUV: return "SDL_PIXELFORMAT_IYUV"; break;
	case SDL_PIXELFORMAT_YUY2: return "SDL_PIXELFORMAT_YUY2"; break;
	case SDL_PIXELFORMAT_UYVY: return "SDL_PIXELFORMAT_UYVY"; break;
	case SDL_PIXELFORMAT_YVYU: return "SDL_PIXELFORMAT_YVYU"; break;
	default : return "SDL_PIXELFORMAT_UNKNOWN"; break;

	}
}


bool wzMain(int &argc, char **argv)
{
	// populate with the saved values (if we had any)
	unsigned	width = pie_GetVideoBufferWidth();
	unsigned	height = pie_GetVideoBufferHeight();
	unsigned	bitDepth = pie_GetVideoBufferDepth();
	unsigned	fsaa = war_getFSAA();

	initKeycodes();
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
	{
		debug( LOG_ERROR, "Error: Could not initialise SDL (%s).\n", SDL_GetError() );
		return false;
	}

	// Populated our resolution list (does all displays now)
	SDL_DisplayMode	displaymode;
	struct screeninfo screenlist;
	for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i)		// How many monitors we got
	{
		int numdisplaymodes = SDL_GetNumDisplayModes(i);	// Get the number of display modes on this monitor
		for (int j = 0; j < numdisplaymodes; j++)
		{
			displaymode.format = displaymode.w = displaymode.h = displaymode.refresh_rate = 0;
			displaymode.driverdata = 0;
			if (SDL_GetDisplayMode(i, j, &displaymode) < 0)
			{
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s\nExiting...\n", SDL_GetError());
				SDL_Quit();
				exit(EXIT_FAILURE);
			}

			debug(LOG_INFO, "[%d]%dx%d %d %s\n", i, displaymode.w, displaymode.h, displaymode.refresh_rate, getSDL_fmt_string(displaymode.format));
			if (displaymode.refresh_rate <= 59) continue;		// only store 60Hz & higher modes some display report 59 on linux ?
			screenlist.height = displaymode.h;
			screenlist.width = displaymode.w;
			screenlist.refresh_rate = displaymode.refresh_rate;
			screenlist.screen = i;

			displaylist.push_back(screenlist);
		}
	}

	SDL_DisplayMode current;
	for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i)
	{
		int display = SDL_GetCurrentDisplayMode(i, &current);
		if (display != 0)
		{
			debug(LOG_FATAL, "Can't get the current display mode, because: %s", SDL_GetError());
			SDL_Quit();
			exit(EXIT_FAILURE);
		}
		debug(LOG_INFO, "[%d]%dx%d %d\n", i, current.w, current.h, current.refresh_rate);
	}
	if (width == 0 || height == 0)
	{
		pie_SetVideoBufferWidth(width = screenWidth = current.w);
		pie_SetVideoBufferHeight(height = screenHeight = current.h);
	}
	else
	{
		screenWidth = width;
		screenHeight = height;
	}
	screenWidth = MAX(screenWidth, 640);
	screenHeight = MAX(screenHeight, 480);

	//// The flags to pass to SDL_CreateWindow
	int video_flags  = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;    // Enable OpenGL in SDL

	if (war_getFullscreen())
	{
		video_flags |= SDL_WINDOW_FULLSCREEN;
	}

	WZwindow = SDL_CreateWindow(PACKAGE_NAME,
		SDL_WINDOWPOS_CENTERED_DISPLAY(0),	//NOTE: at this time, we only show on main monitor, centered
		SDL_WINDOWPOS_CENTERED_DISPLAY(0),	// SDL_WINDOWPOS_CENTERED_DISPLAY(N) is used to move it to another screen
		screenWidth, screenHeight,
		video_flags);

	if (!WZwindow)
	{
		debug(LOG_FATAL, "Can't create a window, because: %s", SDL_GetError());
		SDL_Quit();
		exit(EXIT_FAILURE);
	}

	WZglcontext = SDL_GL_CreateContext(WZwindow);
	if (!WZglcontext)
	{
		debug(LOG_ERROR, "Failed to create a openGL context! [%s]", SDL_GetError());
		return false;
	}
	// Set the double buffer OpenGL attribute.
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	// Enable FSAA anti-aliasing if and at the level requested by the user
	if (fsaa)
	{
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, fsaa);
	}

	int bpp = SDL_BITSPERPIXEL(SDL_GetWindowPixelFormat(WZwindow));
	debug(LOG_INFO, "Bpp = %d format %s" , bpp, getSDL_fmt_string(SDL_GetWindowPixelFormat(WZwindow)));
	if (!bpp)
	{
		debug(LOG_ERROR, "Video mode %dx%d@%dbpp is not supported!", width, height, bitDepth);
		return false;
	}
	switch (bpp)
	{
	case 32:
	case 24:		// all is good...
		break;
	case 16:
		info("Using colour depth of %i instead of a 32/24 bit depth (True color).", bpp);
		info("You will experience graphics glitches!");
		break;
	case 8:
		debug(LOG_FATAL, "You don't want to play Warzone with a bit depth of %i, do you?", bpp);
		SDL_Quit();
		exit(1);
		break;
	default:
		debug(LOG_FATAL, "Unsupported bit depth: %i", bpp);
		exit(1);
		break;
	}

	// Enable/disable vsync if requested by the user
	wzSetSwapInterval(war_GetVsync());

	int value = 0;
	if (SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &value) == -1 || value == 0)
	{
		debug( LOG_FATAL, "OpenGL initialization did not give double buffering!" );
		debug( LOG_FATAL, "Double buffering is required for this game!");
		SDL_Quit();
		exit(1);
	}

#if SDL_BYTEORDER == SDL_BIG_ENDIAN
	uint32_t rmask = 0xff000000;
	uint32_t gmask = 0x00ff0000;
	uint32_t bmask = 0x0000ff00;
	uint32_t amask = 0x000000ff;
#else
	uint32_t rmask = 0x000000ff;
	uint32_t gmask = 0x0000ff00;
	uint32_t bmask = 0x00ff0000;
	uint32_t amask = 0xff000000;
#endif

	SDL_Surface *surface_icon = SDL_CreateRGBSurfaceFrom((void*)wz2100icon.pixel_data, wz2100icon.width, wz2100icon.height, wz2100icon.bytes_per_pixel * 8,
		wz2100icon.width * wz2100icon.bytes_per_pixel, rmask, gmask, bmask, amask);
	if (surface_icon)
	{
		SDL_SetWindowIcon(WZwindow, surface_icon);
		SDL_FreeSurface(surface_icon);
	}
	else
	{
		debug(LOG_ERROR, "Could not set window icon because %s", SDL_GetError());
	}

	SDL_SetWindowTitle(WZwindow, PACKAGE_NAME);

	/* initialise all cursors */
	if (war_GetColouredCursor())
	{
		sdlInitColoredCursors();
	}
	else
	{
		sdlInitCursors();
	}

	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	//float aspect = (float)width / (float)height;
	glOrtho(0, width, height, 0, 1, -1);

	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glCullFace(GL_FRONT);
	glEnable(GL_CULL_FACE);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	appPtr = new QApplication(argc, argv,true);
	return true;

}

/*!
 * Activation (focus change ... and) eventhandler
 */
static void handleActiveEvent(SDL_Event *event)
{
	if (event->type == SDL_WINDOWEVENT)
	{
		switch (event->window.event)
		{
		case SDL_WINDOWEVENT_SHOWN:
			SDL_Log("Window %d shown", event->window.windowID);
			break;
		case SDL_WINDOWEVENT_HIDDEN:
			SDL_Log("Window %d hidden", event->window.windowID);
			break;
		case SDL_WINDOWEVENT_EXPOSED:
			SDL_Log("Window %d exposed", event->window.windowID);
			break;
		case SDL_WINDOWEVENT_MOVED:
			SDL_Log("Window %d moved to %d,%d", event->window.windowID, event->window.data1, event->window.data2);
			break;
		case SDL_WINDOWEVENT_RESIZED:
			SDL_Log("Window %d resized to %dx%d", event->window.windowID, event->window.data1, event->window.data2);
			break;
		case SDL_WINDOWEVENT_MINIMIZED:
			SDL_Log("Window %d minimized", event->window.windowID);
			break;
		case SDL_WINDOWEVENT_MAXIMIZED:
			SDL_Log("Window %d maximized", event->window.windowID);
			break;
		case SDL_WINDOWEVENT_RESTORED:
			SDL_Log("Window %d restored", event->window.windowID);
			break;
		case SDL_WINDOWEVENT_ENTER:
			SDL_Log("Mouse entered window %d", event->window.windowID);
			break;
		case SDL_WINDOWEVENT_LEAVE:
			SDL_Log("Mouse left window %d", event->window.windowID);
			break;
		case SDL_WINDOWEVENT_FOCUS_GAINED:
			mouseInWindow = SDL_TRUE;
			SDL_Log("Window %d gained keyboard focus", event->window.windowID);
			break;
		case SDL_WINDOWEVENT_FOCUS_LOST:
			mouseInWindow = SDL_FALSE;
			SDL_Log("Window %d lost keyboard focus", event->window.windowID);
			break;
		case SDL_WINDOWEVENT_CLOSE:
			SDL_Log("Window %d closed", event->window.windowID);
			break;
		default:
			SDL_Log("Window %d got unknown event %d", event->window.windowID, event->window.event);
			break;
		}
	}
}

// Actual mainloop
void wzMain2(void)
{
	SDL_Event event;

	while (true)
	{
		/* Deal with any windows messages */
		while (SDL_PollEvent(&event))
		{
			switch (event.type)
			{
			case SDL_KEYUP:
			case SDL_KEYDOWN:
				inputHandleKeyEvent(&event.key);
				break;
			case SDL_MOUSEBUTTONUP:
			case SDL_MOUSEBUTTONDOWN:
				inputHandleMouseButtonEvent(&event.button);
				break;
			case SDL_MOUSEMOTION:
				inputHandleMouseMotionEvent(&event.motion);
				break;
			case SDL_MOUSEWHEEL:
				inputHandleMouseWheelEvent(&event.wheel);
				break;
			case SDL_WINDOWEVENT:
				handleActiveEvent(&event);
				break;
			case SDL_TEXTINPUT:	// SDL now handles text input differently
				inputhandleText(&event.text);
				break;
			case SDL_TEXTEDITING:
                    //Update the composition text.  [NOTE: *if* we use TextInput...we have this as well]
                    //Update the cursor position.
                    //Update the selection length (if any).
				break;
			case SDL_QUIT:
				return;
			default:
				break;
			}
		}

		appPtr->processEvents();

		mainLoop();
		inputNewFrame();
	}
}

void wzShutdown()
{
	sdlFreeCursors();
	SDL_DestroyWindow(WZwindow);
	SDL_Quit();
	appPtr->quit();
	delete appPtr;
	appPtr = NULL;
}
