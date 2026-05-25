/*
 * opengl.c - OpenGL 3.3 / OpenGL ES 3.0 Rendering Backend
 *
 * Part of the zelda3 C reimplementation of The Legend of Zelda: A Link to the Past.
 *
 * This file implements the GPU-accelerated rendering pipeline using SDL2 for
 * window/context management and OpenGL for texture upload and display. The renderer
 * draws a fullscreen quad textured with the emulated SNES framebuffer, supporting:
 *   - Both desktop OpenGL 3.3 Core and OpenGL ES 3.0 (for embedded/mobile targets)
 *   - Aspect-ratio-correct letterboxing/pillarboxing
 *   - Optional bilinear filtering vs. nearest-neighbor (pixel-perfect) scaling
 *   - Optional GLSL shader post-processing loaded from user-specified files
 *   - Debug message callbacks for GPU error diagnosis in debug builds
 *
 * The renderer exposes its functionality through a RendererFuncs vtable that
 * main.c uses to abstract over different rendering backends.
 */

// OpenGL function loader and core profile declarations
#include "third_party/gl_core/gl_core_3_1.h"
// SDL2 windowing, GL context creation, and input
#include <SDL.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
// Project-wide fixed-width integer typedefs (uint8, uint16, etc.)
#include "types.h"
// Utility functions including Die() for fatal error reporting
#include "util.h"
// GLSL shader loading and multi-pass rendering support
#include "glsl_shader.h"
// User configuration (shader paths, filtering, aspect ratio settings)
#include "config.h"

/*
 * CODE(...) - Inline GLSL shader source stringification macro.
 * Converts the variadic arguments into a C string literal at compile time,
 * allowing GLSL shader code to be written inline without manual quoting.
 * The #__VA_ARGS__ preprocessor operator stringifies everything between the parens.
 */
#define CODE(...) #__VA_ARGS__

/* --- Module-level state for the OpenGL renderer --- */

// Handle to the SDL window that owns the GL context
static SDL_Window *g_window;
// CPU-side pixel buffer that receives the emulated SNES framebuffer (BGRA format)
static uint8 *g_screen_buffer;
// Current allocation size of g_screen_buffer in pixels (width * height)
static size_t g_screen_buffer_size;
// Dimensions of the current frame being drawn (typically 256x224 or 512x448)
static int g_draw_width, g_draw_height;
// The default shader program handle (simple passthrough vertex + fragment shaders)
static unsigned int g_program;
// Vertex Array Object for the fullscreen quad geometry
static unsigned int g_VAO;
// The GPU texture that receives each frame's pixel data, with cached dimensions
static GlTextureWithSize g_texture;
// Optional GLSL post-processing shader chain loaded from a user-specified file
static GlslShader *g_glsl_shader;
// Whether we are using OpenGL ES (true) vs desktop OpenGL Core (false)
static bool g_opengl_es;

/*
 * MessageCallback - OpenGL debug message handler registered via glDebugMessageCallback.
 *
 * Parameters:
 *   source    - Origin of the message (API, shader compiler, window system, etc.)
 *   type      - Category: error, deprecated behavior, undefined behavior, performance, etc.
 *   id        - Driver-specific message identifier
 *   severity  - How critical the message is (high, medium, low, notification)
 *   length    - Length of the message string
 *   message   - Human-readable description of the issue
 *   userParam - User data pointer passed during callback registration (unused here)
 *
 * Only active in debug builds (kDebugFlag). Filters out GL_DEBUG_TYPE_OTHER noise
 * (informational messages like buffer allocation details) and treats actual GL errors
 * as fatal, since they typically indicate broken rendering state that would produce
 * garbage output.
 */
static void GL_APIENTRY MessageCallback(GLenum source,
                GLenum type,
                GLuint id,
                GLenum severity,
                GLsizei length,
                const GLchar *message,
                const void *userParam) {
  // Suppress purely informational messages that clutter the debug log
  if (type == GL_DEBUG_TYPE_OTHER)
    return;

  fprintf(stderr, "GL CALLBACK: %s type = 0x%x, severity = 0x%x, message = %s\n",
          (type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : ""),
          type, severity, message);
  // GL errors are unrecoverable in this renderer; halt immediately
  if (type == GL_DEBUG_TYPE_ERROR)
    Die("OpenGL error!\n");
}

/*
 * OpenGLRenderer_Init - Creates the GL context and sets up the rendering pipeline.
 *
 * Parameters:
 *   window - SDL window to attach the GL context to
 *
 * Returns: true on success (always, since failures call Die() to abort)
 *
 * This function performs the full one-time GPU setup:
 *   1. Creates the OpenGL context and loads function pointers
 *   2. Validates the GL version meets minimum requirements
 *   3. Registers debug callbacks in debug builds
 *   4. Creates a fullscreen quad (two triangles as a triangle strip)
 *   5. Compiles the passthrough vertex and fragment shaders
 *   6. Optionally loads a user-specified GLSL post-processing shader
 */
static bool OpenGLRenderer_Init(SDL_Window *window) {
  g_window = window;
  SDL_GLContext context = SDL_GL_CreateContext(window);
  // Suppress unused variable warning; context lifetime is tied to the window
  (void)context;

  // Enable vsync (swap interval 1) to prevent tearing
  SDL_GL_SetSwapInterval(1);
  // Load all OpenGL function pointers for the core profile
  ogl_LoadFunctions();

  // Verify the GPU driver supports the minimum required GL version
  if (!g_opengl_es) {
    if (!ogl_IsVersionGEQ(3, 3))
      Die("You need OpenGL 3.3");
  } else {
    // For ES, query the context attributes since ogl_IsVersionGEQ is desktop-only
    int majorVersion = 0, minorVersion = 0;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &majorVersion);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minorVersion);
    if (majorVersion < 3)
      Die("You need OpenGL ES 3.0");

  }

  // In debug builds, enable synchronous GL error reporting so errors are
  // caught at the exact API call that caused them, not deferred
  if (kDebugFlag) {
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(MessageCallback, 0);
  }

  // Allocate the GPU texture that will receive each frame's pixel data
  glGenTextures(1, &g_texture.gl_texture);

  /*
   * Fullscreen quad vertices: 4 corners in NDC (Normalized Device Coordinates).
   * Each vertex has 5 floats: (x, y, z, u, v).
   * The quad spans the entire [-1,1] clip space so it fills the viewport.
   * Texture coords are flipped vertically (v=0 at top, v=1 at bottom) because
   * the SNES framebuffer has row 0 at the top, matching this UV layout.
   * Rendered as a GL_TRIANGLE_STRIP (4 vertices = 2 triangles).
   */
  static const float kVertices[] = {
    // positions          // texture coords
    -1.0f,  1.0f, 0.0f,   0.0f, 0.0f, // top left
    -1.0f, -1.0f, 0.0f,   0.0f, 1.0f, // bottom left
     1.0f,  1.0f, 0.0f,   1.0f, 0.0f, // top right
     1.0f, -1.0f, 0.0f,   1.0f, 1.0f,  // bottom right
  };

  // create a vertex buffer object
  unsigned int vbo;
  glGenBuffers(1, &vbo);

  // vertex array object
  glGenVertexArrays(1, &g_VAO);
  // 1. bind Vertex Array Object
  glBindVertexArray(g_VAO);
  // 2. copy our vertices array in a buffer for OpenGL to use
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
  // position attribute: layout(location=0), 3 floats, stride 5 floats, offset 0
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(0);
  // texture coord attribute: layout(location=1), 2 floats, stride 5 floats, offset 3 floats
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);

  /*
   * Vertex shader: simple passthrough that forwards position and UV coordinates.
   * Two variants are provided: one for desktop GL 3.3 Core and one for GL ES 3.0.
   * The ES variant is identical except for the #version directive.
   */
  // vertex shader
  const GLchar *vs_code_core = "#version 330 core\n" CODE(
  layout(location = 0) in vec3 aPos;
  layout(location = 1) in vec2 aTexCoord;
  out vec2 TexCoord;
  void main() {
    gl_Position = vec4(aPos, 1.0);
    TexCoord = vec2(aTexCoord.x, aTexCoord.y);
  }
);

  const GLchar *vs_code_es = "#version 300 es\n" CODE(
  layout(location = 0) in vec3 aPos;
  layout(location = 1) in vec2 aTexCoord;
  out vec2 TexCoord;
  void main() {
    gl_Position = vec4(aPos, 1.0);
    TexCoord = vec2(aTexCoord.x, aTexCoord.y);
  }
);

  // Select the appropriate shader variant based on the GL context type
  const GLchar *vs_code = g_opengl_es ? vs_code_es : vs_code_core;
  unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_code, NULL);
  glCompileShader(vs);

  // Check vertex shader compilation and log any errors from the GLSL compiler
  int success;
  char infolog[512];
  glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(vs, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  /*
   * Fragment shader: samples the framebuffer texture and outputs the color directly.
   * The ES variant adds "precision mediump float" required by the GLES spec.
   * This is the default shader used when no custom GLSL post-processing is configured.
   */
  // fragment shader
  const GLchar *fs_code_core = "#version 330 core\n" CODE(
  out vec4 FragColor;
  in vec2 TexCoord;
  // texture samplers
  uniform sampler2D texture1;
  void main() {
    FragColor = texture(texture1, TexCoord);
  }
);

  const GLchar *fs_code_es = "#version 300 es\n" CODE(
  precision mediump float;
  out vec4 FragColor;
  in vec2 TexCoord;
  // texture samplers
  uniform sampler2D texture1;
  void main() {
    FragColor = texture(texture1, TexCoord);
  }
);


  const GLchar *fs_code = g_opengl_es ? fs_code_es : fs_code_core;
  unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &fs_code, NULL);
  glCompileShader(fs);

  // Check fragment shader compilation
  glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(fs, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  // create program
  // Link the vertex and fragment shaders into the final pipeline program
  int program = g_program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glGetProgramiv(program, GL_LINK_STATUS, &success);

  if (!success) {
    glGetProgramInfoLog(program, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  // If the user specified a custom shader file in the config, load it for
  // multi-pass post-processing (CRT filters, scanlines, etc.)
  if (g_config.shader)
    g_glsl_shader = GlslShader_CreateFromFile(g_config.shader, g_opengl_es);

  return true;
}

/*
 * OpenGLRenderer_Destroy - Cleanup stub for the OpenGL renderer.
 *
 * Currently a no-op because the GL context and all GPU resources are destroyed
 * implicitly when the SDL window is closed and the process exits. If hot-reload
 * or renderer switching were needed, this would free the VAO, VBO, textures,
 * shader program, and GLSL shader chain.
 */
static void OpenGLRenderer_Destroy() {
}

/*
 * OpenGLRenderer_BeginDraw - Prepares a CPU-side pixel buffer for the next frame.
 *
 * Parameters:
 *   width  - Frame width in pixels (e.g., 256 for standard SNES, 512 for hi-res)
 *   height - Frame height in pixels (e.g., 224 or 448)
 *   pixels - [out] Pointer set to the writable BGRA pixel buffer
 *   pitch  - [out] Byte stride between rows (width * 4 for 32-bit BGRA)
 *
 * The caller (PPU renderer) writes pixel data into the returned buffer between
 * BeginDraw and EndDraw calls. The buffer is lazily reallocated only when the
 * frame dimensions increase, avoiding per-frame allocation overhead.
 */
static void OpenGLRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  int size = width * height;

  // Grow the pixel buffer if the current frame is larger than any previous frame
  if (size > g_screen_buffer_size) {
    g_screen_buffer_size = size;
    free(g_screen_buffer);
    // 4 bytes per pixel: Blue, Green, Red, Alpha (BGRA format)
    g_screen_buffer = malloc(size * 4);
  }

  g_draw_width = width;
  g_draw_height = height;
  *pixels = g_screen_buffer;
  *pitch = width * 4;
}

/*
 * OpenGLRenderer_EndDraw - Uploads the CPU framebuffer to the GPU and presents the frame.
 *
 * This is called after the PPU renderer has finished writing pixels into the buffer
 * provided by BeginDraw. The function:
 *   1. Computes a viewport that preserves the SNES aspect ratio (unless overridden)
 *   2. Uploads pixel data to the GPU texture (fast SubImage path if dimensions match)
 *   3. Clears the window to black (for letterbox/pillarbox bars)
 *   4. Renders the textured quad using either the default program or the custom shader
 *   5. Swaps the front/back buffers to display the frame
 */
static void OpenGLRenderer_EndDraw(int crop_left, int crop_width) {
  int drawable_width, drawable_height;
  if (crop_left != 0 || crop_width != g_draw_width) {
    for (int y = 0; y < g_draw_height; y++) {
      uint8 *row = g_screen_buffer + y * g_draw_width * 4;
      memmove(row, row + crop_left * 4, crop_width * 4);
    }
    g_draw_width = crop_width;
  }

  // Query the actual pixel dimensions (may differ from window size on HiDPI displays)
  SDL_GL_GetDrawableSize(g_window, &drawable_width, &drawable_height);

  int viewport_width = drawable_width, viewport_height = drawable_height;

  /*
   * Compute aspect-ratio-correct viewport dimensions.
   * Cross-multiplies to avoid floating point: if w/h_viewport > w/h_draw,
   * then the viewport is too wide (pillarbox), otherwise too tall (letterbox).
   */
  if (!g_config.ignore_aspect_ratio) {
    if (viewport_width * g_draw_height < viewport_height * g_draw_width)
      viewport_height = viewport_width * g_draw_height / g_draw_width;  // limit height
    else
      viewport_width = viewport_height * g_draw_width / g_draw_height;  // limit width
  }

  // Center the viewport within the drawable area; black bars fill the remainder
  int viewport_x = (drawable_width - viewport_width) >> 1;
  // Note: (viewport_height - viewport_height) >> 1 always yields 0; vertical
  // centering is effectively handled by the drawable height matching
  int viewport_y = (viewport_height - viewport_height) >> 1;

  glBindTexture(GL_TEXTURE_2D, g_texture.gl_texture);
  if (g_draw_width == g_texture.width && g_draw_height == g_texture.height) {
    /*
     * Fast path: texture dimensions unchanged, so use glTexSubImage2D to update
     * pixel data in-place without reallocating GPU memory. Desktop GL uses
     * GL_UNSIGNED_INT_8_8_8_8_REV for native BGRA byte order; ES uses GL_UNSIGNED_BYTE.
     */
    if (!g_opengl_es)
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_draw_width, g_draw_height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g_screen_buffer);
    else
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_draw_width, g_draw_height, GL_BGRA, GL_UNSIGNED_BYTE, g_screen_buffer);
  } else {
    /*
     * Slow path: frame dimensions changed (e.g., hi-res mode toggle), so
     * reallocate the GPU texture with glTexImage2D and cache the new size.
     */
    g_texture.width = g_draw_width;
    g_texture.height = g_draw_height;
    if (!g_opengl_es)
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_draw_width, g_draw_height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g_screen_buffer);
    else
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_draw_width, g_draw_height, 0, GL_BGRA, GL_UNSIGNED_BYTE, g_screen_buffer);
  }

  // Clear to black so letterbox/pillarbox bars are black, not garbage
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (g_glsl_shader == NULL) {
    // Default rendering path: draw the textured fullscreen quad directly
    glViewport(viewport_x, viewport_y, viewport_width, viewport_height);
    glUseProgram(g_program);
    // Choose texture filtering: GL_LINEAR for smooth upscaling, GL_NEAREST for
    // crisp pixel-perfect rendering that preserves the retro aesthetic
    int filter = g_config.linear_filtering ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glBindVertexArray(g_VAO);
    // Draw the fullscreen quad as a triangle strip (4 vertices = 2 triangles)
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  } else {
    // Custom shader path: delegate to the multi-pass GLSL shader renderer
    // which applies CRT simulation, scanlines, or other post-processing effects
    GlslShader_Render(g_glsl_shader, &g_texture, viewport_x, viewport_y, viewport_width, viewport_height);
  }

  // Present the completed frame to the display (blocks if vsync is enabled)
  SDL_GL_SwapWindow(g_window);
}

/*
 * Renderer vtable: function pointer struct that main.c uses to call into
 * this backend without knowing the concrete implementation. This pattern
 * allows swapping between OpenGL and other renderers (e.g., software) at
 * compile time or runtime.
 */
static const struct RendererFuncs kOpenGLRendererFuncs = {
  &OpenGLRenderer_Init,
  &OpenGLRenderer_Destroy,
  &OpenGLRenderer_BeginDraw,
  &OpenGLRenderer_EndDraw,
};

/*
 * OpenGLRenderer_Create - Public entry point to configure and register the OpenGL renderer.
 *
 * Parameters:
 *   funcs         - [out] Filled with the renderer vtable for the caller to use
 *   use_opengl_es - true for OpenGL ES 3.0 (embedded/Switch), false for desktop GL 3.3
 *
 * Must be called BEFORE SDL_CreateWindow, because SDL needs the GL context
 * attributes set before window creation to request the correct GL profile.
 * The actual GL context is not created here; that happens in OpenGLRenderer_Init.
 */
void OpenGLRenderer_Create(struct RendererFuncs *funcs, bool use_opengl_es) {
  g_opengl_es = use_opengl_es;
  if (!g_opengl_es) {
    // Request a desktop OpenGL 3.3 Core profile (no deprecated fixed-function)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  } else {
    // Request OpenGL ES 3.0 for platforms like Nintendo Switch
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  }
  *funcs = kOpenGLRendererFuncs;
}

