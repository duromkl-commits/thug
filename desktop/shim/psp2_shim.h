/*****************************************************************************
**  THUG desktop -- Vita SDK shim                                           **
**  desktop/shim/psp2_shim.h                                                **
**                                                                          **
**  The Vita backend (Code/.../Vita) talks to the console through ~140 Vita   **
**  SDK calls. On desktop every <psp2/...> header lands here, and           **
**  desktop/src/shim_psp2.cpp implements the calls on SDL2 + the C library. **
**  Only what the backend actually uses is declared. Layouts follow         **
**  vitasdk/vita-headers so field names match.                              **
*****************************************************************************/

#ifndef THUG_DESKTOP_PSP2_SHIM_H
#define THUG_DESKTOP_PSP2_SHIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// -- Types --------------------------------------------------------------------
typedef int8_t   SceInt8;
typedef uint8_t  SceUInt8;
typedef int16_t  SceInt16;
typedef uint16_t SceUInt16;
typedef int32_t  SceInt32;
typedef uint32_t SceUInt32;
typedef int32_t  SceInt;
typedef uint32_t SceUInt;
typedef int64_t  SceInt64;
typedef uint64_t SceUInt64;
typedef int      SceUID;
typedef unsigned int SceSize;
typedef int      SceSSize;
typedef int64_t  SceOff;
typedef int      SceMode;
typedef uint64_t SceKernelSysClock;
typedef unsigned int SceKernelCpuMask;
typedef int      SceBool;
typedef void    *ScePVoid;
typedef unsigned int SceUIntPtr;
typedef unsigned int SceKernelMemBlockType;

#define SCE_OK 0
#define SCE_TRUE 1
#define SCE_FALSE 0

typedef struct SceDateTime {
	unsigned short year;
	unsigned short month;
	unsigned short day;
	unsigned short hour;
	unsigned short minute;
	unsigned short second;
	unsigned int microsecond;
} SceDateTime;

// -- Files (io/fcntl.h, io/stat.h, io/dirent.h) -------------------------------
enum {
	SCE_O_RDONLY = 0x0001,
	SCE_O_WRONLY = 0x0002,
	SCE_O_RDWR   = 0x0003,
	SCE_O_NBLOCK = 0x0004,
	SCE_O_APPEND = 0x0100,
	SCE_O_CREAT  = 0x0200,
	SCE_O_TRUNC  = 0x0400,
	SCE_O_EXCL   = 0x0800
};
enum { SCE_SEEK_SET = 0, SCE_SEEK_CUR = 1, SCE_SEEK_END = 2 };

#define SCE_S_IFDIR 0010000
#define SCE_S_IFREG 0020000
#define SCE_S_IFMT  0170000
#define SCE_S_ISDIR(m) (((m) & SCE_S_IFMT) == SCE_S_IFDIR)
#define SCE_S_ISREG(m) (((m) & SCE_S_IFMT) == SCE_S_IFREG)
#define SCE_SO_IFDIR 0x1000
#define SCE_SO_IFREG 0x2000

// glibc's <sys/stat.h> turns st_atime/st_mtime/st_ctime into macros; keep
// the Vita field names intact whatever was included before.
#pragma push_macro("st_atime")
#pragma push_macro("st_mtime")
#pragma push_macro("st_ctime")
#undef st_atime
#undef st_mtime
#undef st_ctime
typedef struct SceIoStat {
	SceMode st_mode;
	unsigned int st_attr;
	SceOff st_size;
	SceDateTime st_ctime;
	SceDateTime st_atime;
	SceDateTime st_mtime;
	unsigned int st_private[6];
} SceIoStat;
#pragma pop_macro("st_atime")
#pragma pop_macro("st_mtime")
#pragma pop_macro("st_ctime")

typedef struct SceIoDirent {
	SceIoStat d_stat;
	char d_name[256];
	void *d_private;
	int dummy;
} SceIoDirent;

SceUID sceIoOpen( const char *file, int flags, SceMode mode );
int    sceIoClose( SceUID fd );
int    sceIoRead( SceUID fd, void *data, SceSize size );
int    sceIoWrite( SceUID fd, const void *data, SceSize size );
SceOff sceIoLseek( SceUID fd, SceOff offset, int whence );
int    sceIoLseek32( SceUID fd, int offset, int whence );
int    sceIoRemove( const char *file );
int    sceIoMkdir( const char *dir, SceMode mode );
int    sceIoRmdir( const char *path );
int    sceIoRename( const char *oldname, const char *newname );
int    sceIoGetstat( const char *file, SceIoStat *stat );
int    sceIoGetstatByFd( SceUID fd, SceIoStat *stat );
int    sceIoSyncByFd( SceUID fd, int flag );
SceUID sceIoDopen( const char *dirname );
int    sceIoDread( SceUID fd, SceIoDirent *dir );
int    sceIoDclose( SceUID fd );
int    sceIoDevctl( const char *dev, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen );

// Maps a Vita path ("ux0:data/thug/...", "app0:...") to a host path.
// Returns the host path in a static thread-local-ish buffer owned by caller.
void   desktop_host_path( const char *vita_path, char *out, size_t out_size );
const char *desktop_data_root( void );
// Shows the message in a dialog (and the log) and exits.
void   desktop_fatal( const char *msg );

// -- Kernel (kernel/processmgr.h, threadmgr.h, sysmem.h, clib.h) -------------
#define SCE_KERNEL_CPU_MASK_USER_0   0x00010000
#define SCE_KERNEL_CPU_MASK_USER_1   0x00020000
#define SCE_KERNEL_CPU_MASK_USER_2   0x00040000
#define SCE_KERNEL_CPU_MASK_USER_ALL 0x00070000
#define SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT 0
#define SCE_KERNEL_DEFAULT_PRIORITY_USER 0x10000100
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 0x0c20d060
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW 0x09408060
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW 0x0c80d060

enum {
	SCE_KERNEL_POWER_TICK_DEFAULT = 0,
	SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND = 1,
	SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF = 4,
	SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING = 6
};

typedef int (*SceKernelThreadEntry)( SceSize args, void *argp );

typedef struct SceKernelThreadOptParam { SceSize size; SceUInt32 attr; } SceKernelThreadOptParam;
typedef struct SceKernelSemaOptParam   { SceSize size; } SceKernelSemaOptParam;
typedef struct SceKernelMutexOptParam  { SceSize size; int ceilingPriority; } SceKernelMutexOptParam;
typedef struct SceKernelLwMutexOptParam { SceSize size; } SceKernelLwMutexOptParam;
typedef struct SceKernelLwMutexWork { int64_t data[4]; } SceKernelLwMutexWork;
typedef struct SceKernelAllocMemBlockOpt { SceSize size; SceUInt32 attr; SceSize alignment; } SceKernelAllocMemBlockOpt;

typedef struct SceKernelFreeMemorySizeInfo {
	int size;
	int size_user;
	int size_cdram;
	int size_phycont;
} SceKernelFreeMemorySizeInfo;

typedef struct SceKernelThreadCpuRegisterInfo { unsigned int reg[16]; unsigned int cpsr; } SceKernelThreadCpuRegisterInfo;
typedef struct SceThreadCpuRegisters { SceKernelThreadCpuRegisterInfo user; SceKernelThreadCpuRegisterInfo kernel; } SceThreadCpuRegisters;

SceUInt64 sceKernelGetProcessTimeWide( void );
SceUInt32 sceKernelGetProcessTimeLow( void );
int    sceKernelGetProcessTime( SceKernelSysClock *clock );
int    sceKernelDelayThread( SceUInt delay );
int    sceKernelDelayThreadCB( SceUInt delay );
SceUID sceKernelCreateThread( const char *name, SceKernelThreadEntry entry, int initPriority,
                              SceSize stackSize, SceUInt attr, int cpuAffinityMask,
                              const SceKernelThreadOptParam *option );
int    sceKernelStartThread( SceUID thid, SceSize arglen, void *argp );
int    sceKernelWaitThreadEnd( SceUID thid, int *stat, SceUInt *timeout );
int    sceKernelDeleteThread( SceUID thid );
int    sceKernelExitDeleteThread( int status );
int    sceKernelGetThreadId( void );
int    sceKernelChangeThreadPriority( SceUID thid, int priority );
int    sceKernelGetThreadCpuRegisters( SceUID thid, SceThreadCpuRegisters *registers );
SceUID sceKernelCreateSema( const char *name, SceUInt attr, int initVal, int maxVal, SceKernelSemaOptParam *option );
int    sceKernelDeleteSema( SceUID semaid );
int    sceKernelSignalSema( SceUID semaid, int signal );
int    sceKernelWaitSema( SceUID semaid, int signal, SceUInt *timeout );
int    sceKernelPollSema( SceUID semaid, int signal );
SceUID sceKernelCreateMutex( const char *name, SceUInt attr, int initCount, SceKernelMutexOptParam *option );
int    sceKernelDeleteMutex( SceUID mutexid );
int    sceKernelLockMutex( SceUID mutexid, int lockCount, unsigned int *timeout );
int    sceKernelTryLockMutex( SceUID mutexid, int lockCount );
int    sceKernelUnlockMutex( SceUID mutexid, int unlockCount );
int    sceKernelCreateLwMutex( SceKernelLwMutexWork *pWork, const char *pName, unsigned int attr, int initCount, const SceKernelLwMutexOptParam *pOptParam );
int    sceKernelDeleteLwMutex( SceKernelLwMutexWork *pWork );
int    sceKernelLockLwMutex( SceKernelLwMutexWork *pWork, int lockCount, unsigned int *pTimeout );
int    sceKernelTryLockLwMutex( SceKernelLwMutexWork *pWork, int lockCount );
int    sceKernelUnlockLwMutex( SceKernelLwMutexWork *pWork, int unlockCount );
int    sceKernelPowerTick( int type );
int    sceKernelExitProcess( int res );
int    sceKernelGetFreeMemorySize( SceKernelFreeMemorySizeInfo *info );
SceUID sceKernelAllocMemBlock( const char *name, SceKernelMemBlockType type, SceSize size, SceKernelAllocMemBlockOpt *opt );
int    sceKernelFreeMemBlock( SceUID uid );
int    sceKernelGetMemBlockBase( SceUID uid, void **basep );
int    sceClibPrintf( const char *fmt, ... );
int    sceClibSnprintf( char *dst, SceSize len, const char *fmt, ... );

// -- Power (power.h) -----------------------------------------------------------
int scePowerSetArmClockFrequency( int freq );
int scePowerSetBusClockFrequency( int freq );
int scePowerSetGpuClockFrequency( int freq );
int scePowerSetGpuXbarClockFrequency( int freq );
int scePowerGetArmClockFrequency( void );
int scePowerGetBusClockFrequency( void );
int scePowerGetGpuClockFrequency( void );
int scePowerGetGpuXbarClockFrequency( void );

// -- Controller (ctrl.h) ---------------------------------------------------------
typedef enum SceCtrlButtons {
	SCE_CTRL_SELECT   = 0x00000001,
	SCE_CTRL_L3       = 0x00000002,
	SCE_CTRL_R3       = 0x00000004,
	SCE_CTRL_START    = 0x00000008,
	SCE_CTRL_UP       = 0x00000010,
	SCE_CTRL_RIGHT    = 0x00000020,
	SCE_CTRL_DOWN     = 0x00000040,
	SCE_CTRL_LEFT     = 0x00000080,
	SCE_CTRL_LTRIGGER = 0x00000100,
	SCE_CTRL_L2       = 0x00000100,
	SCE_CTRL_RTRIGGER = 0x00000200,
	SCE_CTRL_R2       = 0x00000200,
	SCE_CTRL_L1       = 0x00000400,
	SCE_CTRL_R1       = 0x00000800,
	SCE_CTRL_TRIANGLE = 0x00001000,
	SCE_CTRL_CIRCLE   = 0x00002000,
	SCE_CTRL_CROSS    = 0x00004000,
	SCE_CTRL_SQUARE   = 0x00008000,
	SCE_CTRL_INTERCEPTED = 0x00010000,
	SCE_CTRL_PSBUTTON = 0x00010000
} SceCtrlButtons;

typedef enum SceCtrlPadInputMode {
	SCE_CTRL_MODE_DIGITAL = 0,
	SCE_CTRL_MODE_ANALOG = 1,
	SCE_CTRL_MODE_ANALOG_WIDE = 2
} SceCtrlPadInputMode;

typedef struct SceCtrlData {
	uint64_t timeStamp;
	unsigned int buttons;
	unsigned char lx, ly, rx, ry;
	uint8_t up, right, down, left;
	uint8_t lt, rt, l1, r1;
	uint8_t triangle, circle, cross, square;
	uint8_t reserved[4];
} SceCtrlData;

int sceCtrlSetSamplingMode( SceCtrlPadInputMode mode );
int sceCtrlPeekBufferPositive( int port, SceCtrlData *pad_data, int count );
int sceCtrlReadBufferPositive( int port, SceCtrlData *pad_data, int count );

// -- Touch (touch.h) ---------------------------------------------------------
#define SCE_TOUCH_MAX_REPORT 8
typedef enum SceTouchPortType { SCE_TOUCH_PORT_FRONT = 0, SCE_TOUCH_PORT_BACK = 1 } SceTouchPortType;
typedef enum SceTouchSamplingState { SCE_TOUCH_SAMPLING_STATE_STOP = 0, SCE_TOUCH_SAMPLING_STATE_START = 1 } SceTouchSamplingState;
typedef struct SceTouchPanelInfo {
	SceInt16 minAaX, minAaY, maxAaX, maxAaY;
	SceInt16 minDispX, minDispY, maxDispX, maxDispY;
	SceUInt8 minForce, maxForce;
	SceUInt8 reserved[30];
} SceTouchPanelInfo;
typedef struct SceTouchReport {
	SceUInt8 id;
	SceUInt8 force;
	SceInt16 x;
	SceInt16 y;
	SceUInt8 reserved[8];
	SceUInt16 info;
} SceTouchReport;
typedef struct SceTouchData {
	SceUInt64 timeStamp;
	SceUInt32 status;
	SceUInt32 reportNum;
	SceTouchReport report[SCE_TOUCH_MAX_REPORT];
} SceTouchData;
int sceTouchSetSamplingState( SceUInt32 port, SceTouchSamplingState state );
int sceTouchPeek( SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs );
int sceTouchGetPanelInfo( SceUInt32 port, SceTouchPanelInfo *pPanelInfo );

// -- Audio (audioout.h) ----------------------------------------------------------
typedef enum SceAudioOutPortType {
	SCE_AUDIO_OUT_PORT_TYPE_MAIN  = 0,
	SCE_AUDIO_OUT_PORT_TYPE_BGM   = 1,
	SCE_AUDIO_OUT_PORT_TYPE_VOICE = 2
} SceAudioOutPortType;
typedef enum SceAudioOutMode { SCE_AUDIO_OUT_MODE_MONO = 0, SCE_AUDIO_OUT_MODE_STEREO = 1 } SceAudioOutMode;
typedef enum SceAudioOutChannelFlag { SCE_AUDIO_VOLUME_FLAG_L_CH = 1, SCE_AUDIO_VOLUME_FLAG_R_CH = 2 } SceAudioOutChannelFlag;
#define SCE_AUDIO_OUT_MAX_VOL 32768
#define SCE_AUDIO_VOLUME_0DB  SCE_AUDIO_OUT_MAX_VOL
int sceAudioOutOpenPort( SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode );
int sceAudioOutReleasePort( int port );
int sceAudioOutOutput( int port, const void *buf );
int sceAudioOutSetVolume( int port, SceAudioOutChannelFlag ch, int *vol );
int sceAudioOutGetRestSample( int port );

// -- Display / RTC / sysmodule / net ----------------------------------------
int sceDisplayWaitVblankStart( void );
int sceDisplayWaitVblankStartMulti( unsigned int vcount );
int sceRtcGetCurrentClockLocalTime( SceDateTime *time );
int sceSysmoduleLoadModule( SceUInt16 id );
#define SCE_SYSMODULE_NET 0x0001

int sceNetInit( void *param );
int sceNetCtlInit( void );
int sceNetSocket( const char *name, int domain, int type, int protocol );
int sceNetSocketClose( int s );
int sceNetBind( int s, const void *addr, unsigned int addrlen );
int sceNetListen( int s, int backlog );
int sceNetAccept( int s, void *addr, unsigned int *addrlen );
int sceNetSend( int s, const void *msg, unsigned int len, int flags );
int sceNetRecv( int s, void *buf, unsigned int len, int flags );
int sceNetSetsockopt( int s, int level, int optname, const void *optval, unsigned int optlen );
unsigned short sceNetHtons( unsigned short n );

// -- GXM: types only. The direct GXM paths are replaced on desktop
// (Code/Gfx/Vita/p_shader_desktop.inc); these keep declarations compiling.
// SceGxmTexture is a handle to a GL texture, see vglGetGxmTexture.
typedef struct SceGxmTexture { unsigned int gl_name; unsigned int width, height, format; } SceGxmTexture;
typedef struct SceGxmContext SceGxmContext;
typedef struct SceGxmProgram SceGxmProgram;
typedef struct SceGxmProgramParameter SceGxmProgramParameter;
typedef struct SceGxmVertexProgram SceGxmVertexProgram;
typedef struct SceGxmFragmentProgram SceGxmFragmentProgram;
typedef struct SceGxmShaderPatcher SceGxmShaderPatcher;
typedef unsigned int SceGxmShaderPatcherId;
typedef struct SceGxmPrecomputedFragmentState { unsigned int data[9]; } SceGxmPrecomputedFragmentState;
typedef struct SceGxmPrecomputedDraw { unsigned int data[11]; } SceGxmPrecomputedDraw;
typedef struct SceGxmCommandList { unsigned int words[8]; } SceGxmCommandList;
typedef enum SceGxmMultisampleMode { SCE_GXM_MULTISAMPLE_NONE = 0, SCE_GXM_MULTISAMPLE_2X = 1, SCE_GXM_MULTISAMPLE_4X = 2 } SceGxmMultisampleMode;
typedef enum SceGxmColorSurfaceGammaMode { SCE_GXM_COLOR_SURFACE_GAMMA_NONE = 0 } SceGxmColorSurfaceGammaMode;
typedef enum SceGxmAttributeFormat { SCE_GXM_ATTRIBUTE_FORMAT_U8N = 5, SCE_GXM_ATTRIBUTE_FORMAT_F32 = 15 } SceGxmAttributeFormat;
typedef int SceGxmTextureFormat;
enum { SCE_GXM_INDEX_FORMAT_U16 = 0 };
enum { SCE_GXM_PRIMITIVE_TRIANGLES = 0, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP = 1 };
enum { SCE_GXM_INDEX_SOURCE_INDEX_16BIT = 0 };
enum { SCE_GXM_REGION_CLIP_NONE = 0, SCE_GXM_REGION_CLIP_ALL = 1, SCE_GXM_REGION_CLIP_OUTSIDE = 2, SCE_GXM_REGION_CLIP_INSIDE = 3 };
#define SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE 2048
typedef struct SceGxmVertexAttribute { unsigned short streamIndex; unsigned short offset; unsigned char format; unsigned char componentCount; unsigned short regIndex; } SceGxmVertexAttribute;
typedef struct SceGxmVertexStream { unsigned short stride; unsigned short indexSource; } SceGxmVertexStream;
typedef struct SceGxmBlendInfo { unsigned char colorMask, colorFunc, alphaFunc, colorSrc, colorDst, alphaSrc, alphaDst; } SceGxmBlendInfo;
typedef struct SceGxmDeferredContextParams { void *hostMem; SceSize hostMemSize; } SceGxmDeferredContextParams;

unsigned int sceGxmTextureGetWidth( const SceGxmTexture *texture );
unsigned int sceGxmTextureGetHeight( const SceGxmTexture *texture );
SceGxmTextureFormat sceGxmTextureGetFormat( const SceGxmTexture *texture );

// Region clip: the shadow pass restricts drawing to a rectangle. Desktop
// implements it with the scissor test.
int sceGxmSetRegionClip( SceGxmContext *context, int mode, unsigned int xMin, unsigned int yMin,
                         unsigned int xMax, unsigned int yMax );
extern SceGxmContext *gxm_context;

#ifdef __cplusplus
}
#endif

#endif // THUG_DESKTOP_PSP2_SHIM_H
