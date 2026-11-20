#pragma once
// our own minimal GL ES 3.0 / EGL types + enums, only what we touch.
// values checked against the khronos headers.
#include <stdint.h>
#include <stddef.h>

typedef unsigned int  GLenum;
typedef unsigned char GLboolean;
typedef unsigned int  GLbitfield;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned int  GLuint;
typedef float         GLfloat;
typedef float         GLclampf;
typedef char          GLchar;
typedef unsigned char GLubyte;
typedef intptr_t      GLintptr;
typedef intptr_t      GLsizeiptr;

#define GL_FALSE 0
#define GL_TRUE  1
#define GL_NONE  0
#define GL_ZERO  0
#define GL_ONE   1

// errors
#define GL_NO_ERROR                      0
#define GL_INVALID_ENUM                  0x0500
#define GL_INVALID_VALUE                 0x0501
#define GL_INVALID_OPERATION             0x0502
#define GL_OUT_OF_MEMORY                 0x0505
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506

// clear bits
#define GL_DEPTH_BUFFER_BIT   0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT   0x00004000

// prims
#define GL_POINTS         0x0000
#define GL_LINES          0x0001
#define GL_LINE_LOOP      0x0002
#define GL_LINE_STRIP     0x0003
#define GL_TRIANGLES      0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN   0x0006

// blend
#define GL_SRC_COLOR                0x0300
#define GL_ONE_MINUS_SRC_COLOR      0x0301
#define GL_SRC_ALPHA                0x0302
#define GL_ONE_MINUS_SRC_ALPHA      0x0303
#define GL_DST_ALPHA                0x0304
#define GL_ONE_MINUS_DST_ALPHA      0x0305
#define GL_DST_COLOR                0x0306
#define GL_ONE_MINUS_DST_COLOR      0x0307
#define GL_SRC_ALPHA_SATURATE       0x0308
#define GL_CONSTANT_COLOR           0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_CONSTANT_ALPHA           0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define GL_FUNC_ADD                 0x8006
#define GL_MIN                      0x8007
#define GL_MAX                      0x8008
#define GL_FUNC_SUBTRACT            0x800A
#define GL_FUNC_REVERSE_SUBTRACT    0x800B

// buffers
#define GL_ARRAY_BUFFER              0x8892
#define GL_ELEMENT_ARRAY_BUFFER      0x8893
#define GL_PIXEL_PACK_BUFFER         0x88EB
#define GL_PIXEL_UNPACK_BUFFER       0x88EC
#define GL_UNIFORM_BUFFER            0x8A11
#define GL_COPY_READ_BUFFER          0x8F36
#define GL_COPY_WRITE_BUFFER         0x8F37
#define GL_TRANSFORM_FEEDBACK_BUFFER 0x8C8E
#define GL_STREAM_DRAW               0x88E0
#define GL_STATIC_DRAW               0x88E4
#define GL_DYNAMIC_DRAW              0x88E8

// faces / caps
#define GL_FRONT                    0x0404
#define GL_BACK                     0x0405
#define GL_FRONT_AND_BACK           0x0408
#define GL_CW                       0x0900
#define GL_CCW                      0x0901
#define GL_TEXTURE_2D               0x0DE1
#define GL_CULL_FACE                0x0B44
#define GL_BLEND                    0x0BE2
#define GL_DITHER                   0x0BD0
#define GL_STENCIL_TEST             0x0B90
#define GL_DEPTH_TEST               0x0B71
#define GL_SCISSOR_TEST             0x0C11
#define GL_POLYGON_OFFSET_FILL      0x8037
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_SAMPLE_COVERAGE          0x80A0
#define GL_RASTERIZER_DISCARD       0x8C89
#define GL_PRIMITIVE_RESTART_FIXED_INDEX 0x8D69

// funcs
#define GL_NEVER    0x0200
#define GL_LESS     0x0201
#define GL_EQUAL    0x0202
#define GL_LEQUAL   0x0203
#define GL_GREATER  0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL   0x0206
#define GL_ALWAYS   0x0207

// stencil ops
#define GL_KEEP      0x1E00
#define GL_REPLACE   0x1E01
#define GL_INCR      0x1E02
#define GL_DECR      0x1E03
#define GL_INVERT    0x150A
#define GL_INCR_WRAP 0x8507
#define GL_DECR_WRAP 0x8508

// types
#define GL_BYTE           0x1400
#define GL_UNSIGNED_BYTE  0x1401
#define GL_SHORT          0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT            0x1404
#define GL_UNSIGNED_INT   0x1405
#define GL_FLOAT          0x1406
#define GL_HALF_FLOAT     0x140B
#define GL_FIXED          0x140C
#define GL_HALF_FLOAT_OES 0x8D61
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_SHORT_5_6_5   0x8363
#define GL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#define GL_UNSIGNED_INT_24_8      0x84FA
#define GL_INT_2_10_10_10_REV     0x8D9F

// pixel formats
#define GL_DEPTH_COMPONENT 0x1902
#define GL_RED             0x1903
#define GL_ALPHA           0x1906
#define GL_RGB             0x1907
#define GL_RGBA            0x1908
#define GL_LUMINANCE       0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_RG              0x8227
#define GL_DEPTH_STENCIL   0x84F9
#define GL_BGRA_EXT        0x80E1
#define GL_RED_INTEGER     0x8D94
#define GL_RGBA_INTEGER    0x8D99

// sized internal formats
#define GL_R8              0x8229
#define GL_RG8             0x822B
#define GL_R16F            0x822D
#define GL_R32F            0x822E
#define GL_RG16F           0x822F
#define GL_RGB8            0x8051
#define GL_RGBA4           0x8056
#define GL_RGB5_A1         0x8057
#define GL_RGBA8           0x8058
#define GL_RGB10_A2        0x8059
#define GL_RGB565          0x8D62
#define GL_SRGB8           0x8C41
#define GL_SRGB8_ALPHA8    0x8C43
#define GL_RGBA16F         0x881A
#define GL_RGB16F          0x881B
#define GL_RGBA32F         0x8814
#define GL_R11F_G11F_B10F  0x8C3A
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_DEPTH24_STENCIL8  0x88F0
#define GL_DEPTH32F_STENCIL8 0x8CAD
#define GL_STENCIL_INDEX8    0x8D48
#define GL_BGRA8_EXT         0x93A1

// compressed
#define GL_ETC1_RGB8_OES                       0x8D64
#define GL_COMPRESSED_R11_EAC                  0x9270
#define GL_COMPRESSED_SIGNED_R11_EAC           0x9271
#define GL_COMPRESSED_RG11_EAC                 0x9272
#define GL_COMPRESSED_SIGNED_RG11_EAC          0x9273
#define GL_COMPRESSED_RGB8_ETC2                0x9274
#define GL_COMPRESSED_SRGB8_ETC2               0x9275
#define GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2  0x9276
#define GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2 0x9277
#define GL_COMPRESSED_RGBA8_ETC2_EAC           0x9278
#define GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC    0x9279

// textures
#define GL_TEXTURE_CUBE_MAP            0x8513
#define GL_TEXTURE_BINDING_CUBE_MAP    0x8514
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z 0x851A
#define GL_TEXTURE_3D                  0x806F
#define GL_TEXTURE_2D_ARRAY            0x8C1A
#define GL_TEXTURE0                    0x84C0
#define GL_TEXTURE_MAG_FILTER          0x2800
#define GL_TEXTURE_MIN_FILTER          0x2801
#define GL_TEXTURE_WRAP_S              0x2802
#define GL_TEXTURE_WRAP_T              0x2803
#define GL_TEXTURE_WRAP_R              0x8072
#define GL_TEXTURE_MIN_LOD             0x813A
#define GL_TEXTURE_MAX_LOD             0x813B
#define GL_TEXTURE_BASE_LEVEL          0x813C
#define GL_TEXTURE_MAX_LEVEL           0x813D
#define GL_TEXTURE_COMPARE_MODE        0x884C
#define GL_TEXTURE_COMPARE_FUNC        0x884D
#define GL_TEXTURE_SWIZZLE_R           0x8E42
#define GL_TEXTURE_SWIZZLE_A           0x8E45
#define GL_TEXTURE_MAX_ANISOTROPY_EXT  0x84FE
#define GL_NEAREST                     0x2600
#define GL_LINEAR                      0x2601
#define GL_NEAREST_MIPMAP_NEAREST      0x2700
#define GL_LINEAR_MIPMAP_NEAREST       0x2701
#define GL_NEAREST_MIPMAP_LINEAR       0x2702
#define GL_LINEAR_MIPMAP_LINEAR        0x2703
#define GL_REPEAT                      0x2901
#define GL_CLAMP_TO_EDGE               0x812F
#define GL_MIRRORED_REPEAT             0x8370

// pixel store
#define GL_UNPACK_ROW_LENGTH  0x0CF2
#define GL_UNPACK_SKIP_ROWS   0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_UNPACK_ALIGNMENT   0x0CF5
#define GL_PACK_ROW_LENGTH    0x0D02
#define GL_PACK_SKIP_ROWS     0x0D03
#define GL_PACK_SKIP_PIXELS   0x0D04
#define GL_PACK_ALIGNMENT     0x0D05
#define GL_UNPACK_IMAGE_HEIGHT 0x806E
#define GL_UNPACK_SKIP_IMAGES  0x806D

// framebuffers
#define GL_FRAMEBUFFER                 0x8D40
#define GL_RENDERBUFFER                0x8D41
#define GL_READ_FRAMEBUFFER            0x8CA8
#define GL_DRAW_FRAMEBUFFER            0x8CA9
#define GL_COLOR_ATTACHMENT0           0x8CE0
#define GL_DEPTH_ATTACHMENT            0x8D00
#define GL_STENCIL_ATTACHMENT          0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT    0x821A
#define GL_COLOR                       0x1800
#define GL_DEPTH                       0x1801
#define GL_STENCIL                     0x1802
#define GL_FRAMEBUFFER_COMPLETE        0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#define GL_FRAMEBUFFER_UNSUPPORTED     0x8CDD

// shaders
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_VERTEX_SHADER        0x8B31
#define GL_SHADER_TYPE          0x8B4F
#define GL_DELETE_STATUS        0x8B80
#define GL_COMPILE_STATUS       0x8B81
#define GL_LINK_STATUS          0x8B82
#define GL_VALIDATE_STATUS      0x8B83
#define GL_INFO_LOG_LENGTH      0x8B84
#define GL_ATTACHED_SHADERS     0x8B85
#define GL_ACTIVE_UNIFORMS      0x8B86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH 0x8B87
#define GL_SHADER_SOURCE_LENGTH 0x8B88
#define GL_ACTIVE_ATTRIBUTES    0x8B89
#define GL_ACTIVE_ATTRIBUTE_MAX_LENGTH 0x8B8A
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#define GL_ACTIVE_UNIFORM_BLOCKS 0x8A36

// strings
#define GL_VENDOR                   0x1F00
#define GL_RENDERER                 0x1F01
#define GL_VERSION                  0x1F02
#define GL_EXTENSIONS               0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

// getters
#define GL_LINE_WIDTH                  0x0B21
#define GL_VIEWPORT                    0x0BA2
#define GL_SCISSOR_BOX                 0x0C10
#define GL_COLOR_WRITEMASK             0x0C23
#define GL_DEPTH_WRITEMASK             0x0B72
#define GL_MAX_TEXTURE_SIZE            0x0D33
#define GL_MAX_VIEWPORT_DIMS           0x0D3A
#define GL_SUBPIXEL_BITS               0x0D50
#define GL_RED_BITS                    0x0D52
#define GL_GREEN_BITS                  0x0D53
#define GL_BLUE_BITS                   0x0D54
#define GL_ALPHA_BITS                  0x0D55
#define GL_DEPTH_BITS                  0x0D56
#define GL_STENCIL_BITS                0x0D57
#define GL_MAX_RENDERBUFFER_SIZE       0x84E8
#define GL_MAX_CUBE_MAP_TEXTURE_SIZE   0x851C
#define GL_MAX_VERTEX_ATTRIBS          0x8869
#define GL_MAX_TEXTURE_IMAGE_UNITS     0x8872
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS   0x8B4C
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_MAX_VERTEX_UNIFORM_VECTORS  0x8DFB
#define GL_MAX_VARYING_VECTORS         0x8DFC
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS 0x8DFD
#define GL_MAX_VERTEX_UNIFORM_COMPONENTS 0x8B4A
#define GL_MAX_FRAGMENT_UNIFORM_COMPONENTS 0x8B49
#define GL_MAX_VARYING_COMPONENTS      0x8B4B
#define GL_MAX_SAMPLES                 0x8D57
#define GL_MAX_COLOR_ATTACHMENTS       0x8CDF
#define GL_MAX_DRAW_BUFFERS            0x8824
#define GL_MAX_ELEMENTS_VERTICES       0x80E8
#define GL_MAX_ELEMENTS_INDICES        0x80E9
#define GL_MAX_3D_TEXTURE_SIZE         0x8073
#define GL_MAX_ARRAY_TEXTURE_LAYERS    0x88FF
#define GL_MAX_TEXTURE_LOD_BIAS        0x84FD
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS 0x86A2
#define GL_COMPRESSED_TEXTURE_FORMATS  0x86A3
#define GL_NUM_SHADER_BINARY_FORMATS   0x8DF9
#define GL_NUM_PROGRAM_BINARY_FORMATS  0x87FE
#define GL_NUM_EXTENSIONS              0x821D
#define GL_MAJOR_VERSION               0x821B
#define GL_MINOR_VERSION               0x821C
#define GL_SAMPLES                     0x80A9
#define GL_SAMPLE_BUFFERS              0x80A8
#define GL_ALIASED_POINT_SIZE_RANGE    0x846D
#define GL_ALIASED_LINE_WIDTH_RANGE    0x846E
#define GL_FRAMEBUFFER_BINDING         0x8CA6
#define GL_RENDERBUFFER_BINDING        0x8CA7
#define GL_TEXTURE_BINDING_2D          0x8069
#define GL_ARRAY_BUFFER_BINDING        0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_CURRENT_PROGRAM             0x8B8D
#define GL_ACTIVE_TEXTURE              0x84E0
#define GL_IMPLEMENTATION_COLOR_READ_TYPE   0x8B9A
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#define GL_MAX_UNIFORM_BUFFER_BINDINGS 0x8A2F
#define GL_MAX_UNIFORM_BLOCK_SIZE      0x8A30
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT 0x8A34
#define GL_MAX_VERTEX_UNIFORM_BLOCKS   0x8A2B
#define GL_MAX_FRAGMENT_UNIFORM_BLOCKS 0x8A2D
#define GL_MAX_COMBINED_UNIFORM_BLOCKS 0x8A2E
#define GL_MAX_VERTEX_OUTPUT_COMPONENTS 0x9122
#define GL_MAX_FRAGMENT_INPUT_COMPONENTS 0x9125
#define GL_MAX_PROGRAM_TEXEL_OFFSET    0x8905
#define GL_MIN_PROGRAM_TEXEL_OFFSET    0x8904
#define GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS 0x8C80
#define GL_MAX_ELEMENT_INDEX           0x8D6B
#define GL_MAX_SERVER_WAIT_TIMEOUT     0x9111
#define GL_GENERATE_MIPMAP_HINT        0x8192

// ---- EGL ----
typedef unsigned int EGLBoolean;
typedef int32_t      EGLint;
typedef unsigned int EGLenum;
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLNativeDisplayType;
typedef void *EGLNativeWindowType;

#define EGL_FALSE 0
#define EGL_TRUE  1
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)

#define EGL_SUCCESS           0x3000
#define EGL_BAD_ATTRIBUTE     0x3004
#define EGL_BAD_PARAMETER     0x300C
#define EGL_BUFFER_SIZE       0x3020
#define EGL_ALPHA_SIZE        0x3021
#define EGL_BLUE_SIZE         0x3022
#define EGL_GREEN_SIZE        0x3023
#define EGL_RED_SIZE          0x3024
#define EGL_DEPTH_SIZE        0x3025
#define EGL_STENCIL_SIZE      0x3026
#define EGL_CONFIG_CAVEAT     0x3027
#define EGL_CONFIG_ID         0x3028
#define EGL_LEVEL             0x3029
#define EGL_MAX_PBUFFER_HEIGHT 0x302A
#define EGL_MAX_PBUFFER_PIXELS 0x302B
#define EGL_MAX_PBUFFER_WIDTH 0x302C
#define EGL_NATIVE_RENDERABLE 0x302D
#define EGL_NATIVE_VISUAL_ID  0x302E
#define EGL_NATIVE_VISUAL_TYPE 0x302F
#define EGL_SAMPLES           0x3031
#define EGL_SAMPLE_BUFFERS    0x3032
#define EGL_SURFACE_TYPE      0x3033
#define EGL_TRANSPARENT_TYPE  0x3034
#define EGL_TRANSPARENT_BLUE_VALUE  0x3035
#define EGL_TRANSPARENT_GREEN_VALUE 0x3036
#define EGL_TRANSPARENT_RED_VALUE   0x3037
#define EGL_NONE              0x3038
#define EGL_BIND_TO_TEXTURE_RGB  0x3039
#define EGL_BIND_TO_TEXTURE_RGBA 0x303A
#define EGL_MIN_SWAP_INTERVAL 0x303B
#define EGL_MAX_SWAP_INTERVAL 0x303C
#define EGL_LUMINANCE_SIZE    0x303D
#define EGL_ALPHA_MASK_SIZE   0x303E
#define EGL_COLOR_BUFFER_TYPE 0x303F
#define EGL_RENDERABLE_TYPE   0x3040
#define EGL_CONFORMANT        0x3042
#define EGL_HEIGHT            0x3056
#define EGL_WIDTH             0x3057
#define EGL_DRAW              0x3059
#define EGL_READ              0x305A
#define EGL_RGB_BUFFER        0x308E
#define EGL_RENDER_BUFFER     0x3086
#define EGL_BACK_BUFFER       0x3084
#define EGL_SWAP_BEHAVIOR     0x3093
#define EGL_BUFFER_DESTROYED  0x3095
#define EGL_HORIZONTAL_RESOLUTION 0x3090
#define EGL_VERTICAL_RESOLUTION   0x3091
#define EGL_PIXEL_ASPECT_RATIO    0x3092
#define EGL_MULTISAMPLE_RESOLVE   0x3099
#define EGL_MULTISAMPLE_RESOLVE_DEFAULT 0x309A
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_WINDOW_BIT        0x0004
#define EGL_PBUFFER_BIT       0x0001
#define EGL_OPENGL_ES2_BIT    0x0004
#define EGL_OPENGL_ES3_BIT    0x0040
#define EGL_UNKNOWN           ((EGLint)-1)
