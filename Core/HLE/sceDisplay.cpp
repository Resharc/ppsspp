// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <vector>
#include <map>
#include <cmath>
#include <algorithm>

// TODO: Move this somewhere else, cleanup.
#ifndef _WIN32
#include <unistd.h>
#include <sys/time.h>
#endif

// TODO: Move the relevant parts into common. Don't want the core
// to be dependent on "native", I think. Or maybe should get rid of common
// and move everything into native...
#include "base/logging.h"
#include "base/timeutil.h"
#include "profiler/profiler.h"

#include "gfx_es2/gpu_features.h"

#include "Common/ChunkFile.h"
#include "Core/CoreTiming.h"
#include "Core/CoreParameter.h"
#include "Core/Reporting.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceDisplay.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceKernelInterrupt.h"

#include "GPU/GPU.h"
#include "GPU/GPUState.h"
#include "GPU/GPUInterface.h"
#include "GPU/Common/FramebufferCommon.h"

struct FrameBufferState {
	u32 topaddr;
	GEBufferFormat pspFramebufFormat;
	int pspFramebufLinesize;
};

struct WaitVBlankInfo {
	WaitVBlankInfo(u32 tid) : threadID(tid), vcountUnblock(1) {}
	WaitVBlankInfo(u32 tid, int vcount) : threadID(tid), vcountUnblock(vcount) {}
	SceUID threadID;
	// Number of vcounts to block for.
	int vcountUnblock;

	void DoState(PointerWrap &p) {
		auto s = p.Section("WaitVBlankInfo", 1);
		if (!s)
			return;

		p.Do(threadID);
		p.Do(vcountUnblock);
	}
};

// STATE BEGIN
static FrameBufferState framebuf;
static FrameBufferState latchedFramebuf;
static bool framebufIsLatched;

static int enterVblankEvent = -1;
static int leaveVblankEvent = -1;
static int afterFlipEvent = -1;
static int lagSyncEvent = -1;

static double lastLagSync = 0.0;
static bool lagSyncScheduled = false;

// hCount is computed now.
static int vCount;
// The "AccumulatedHcount" can be adjusted, this is the base.
static u32 hCountBase;
static int isVblank;
static int numSkippedFrames;
static bool hasSetMode;
static int resumeMode;
static int holdMode;
static int brightnessLevel;
static int mode;
static int width;
static int height;
static bool wasPaused;

// 1.001f to compensate for the classic 59.94 NTSC framerate that the PSP seems to have.
static const double timePerVblank = 1.001f / 60.0f;

// Don't include this in the state, time increases regardless of state.
static double curFrameTime;
static double lastFrameTime;
static double nextFrameTime;
static int numVBlanksSinceFlip;

static u64 frameStartTicks;
const int hCountPerVblank = 286;

const int PSP_DISPLAY_MODE_LCD = 0;

std::vector<WaitVBlankInfo> vblankWaitingThreads;
// Key is the callback id it was for, or if no callback, the thread id.
// Value is the goal vcount number (in case the callback takes >= 1 vcount to return.)
std::map<SceUID, int> vblankPausedWaits;

// STATE END

// Called when vblank happens (like an internal interrupt.)  Not part of state, should be static.
std::vector<VblankCallback> vblankListeners;

// The vblank period is 731.5 us (0.7315 ms)
const double vblankMs = 0.7315;
const double frameMs = 1001.0 / 60.0;

enum {
	PSP_DISPLAY_SETBUF_IMMEDIATE = 0,
	PSP_DISPLAY_SETBUF_NEXTFRAME = 1
};

static int lastFpsFrame = 0;
static double lastFpsTime = 0.0;
static double fps = 0.0;
static double fpsHistory[120];
static size_t fpsHistoryPos = 0;
static size_t fpsHistoryValid = 0;
static int lastNumFlips = 0;
static float flips = 0.0f;
static int actualFlips = 0;  // taking frameskip into account
static int lastActualFlips = 0;
static float actualFps = 0;
static u64 lastFlipCycles = 0;
// For the "max 60 fps" setting.
static int lastFlipsTooFrequent = 0;

void hleEnterVblank(u64 userdata, int cyclesLate);
void hleLeaveVblank(u64 userdata, int cyclesLate);
void hleAfterFlip(u64 userdata, int cyclesLate);
void hleLagSync(u64 userdata, int cyclesLate);

void __DisplayVblankBeginCallback(SceUID threadID, SceUID prevCallbackId);
void __DisplayVblankEndCallback(SceUID threadID, SceUID prevCallbackId);
int __DisplayGetFlipCount() { return actualFlips; }
int __DisplayGetVCount() { return vCount; }

static void ScheduleLagSync(int over = 0) {
	lagSyncScheduled = g_Config.bForceLagSync;
	if (lagSyncScheduled) {
		CoreTiming::ScheduleEvent(usToCycles(1000 + over), lagSyncEvent, 0);
		lastLagSync = real_time_now();
	}
}

void __DisplayInit() {
	gpuStats.Reset();
	hasSetMode = false;
	mode = 0;
	resumeMode = 0;
	holdMode = 0;
	brightnessLevel = 100;
	width = 480;
	height = 272;
	numSkippedFrames = 0;
	numVBlanksSinceFlip = 0;
	framebufIsLatched = false;
	framebuf.topaddr = 0x04000000;
	framebuf.pspFramebufFormat = GE_FORMAT_8888;
	framebuf.pspFramebufLinesize = 480; // ??
	memset(&latchedFramebuf, 0, sizeof(latchedFramebuf));
	lastFlipCycles = 0;
	lastFlipsTooFrequent = 0;
	wasPaused = false;

	enterVblankEvent = CoreTiming::RegisterEvent("EnterVBlank", &hleEnterVblank);
	leaveVblankEvent = CoreTiming::RegisterEvent("LeaveVBlank", &hleLeaveVblank);
	afterFlipEvent = CoreTiming::RegisterEvent("AfterFlip", &hleAfterFlip);

	lagSyncEvent = CoreTiming::RegisterEvent("LagSync", &hleLagSync);
	ScheduleLagSync();

	CoreTiming::ScheduleEvent(msToCycles(frameMs - vblankMs), enterVblankEvent, 0);
	isVblank = 0;
	frameStartTicks = 0;
	vCount = 0;
	hCountBase = 0;
	curFrameTime = 0.0;
	nextFrameTime = 0.0;
	lastFrameTime = 0.0;

	flips = 0;
	fps = 0.0;
	actualFlips = 0;
	lastActualFlips = 0;
	lastNumFlips = 0;
	fpsHistoryValid = 0;
	fpsHistoryPos = 0;

	__KernelRegisterWaitTypeFuncs(WAITTYPE_VBLANK, __DisplayVblankBeginCallback, __DisplayVblankEndCallback);
}

void __DisplayDoState(PointerWrap &p) {
	auto s = p.Section("sceDisplay", 1, 6);
	if (!s)
		return;

	p.Do(framebuf);
	p.Do(latchedFramebuf);
	p.Do(framebufIsLatched);
	p.Do(frameStartTicks);
	p.Do(vCount);
	if (s <= 2) {
		double oldHCountBase;
		p.Do(oldHCountBase);
		hCountBase = (int) oldHCountBase;
	} else {
		p.Do(hCountBase);
	}
	p.Do(isVblank);
	p.Do(hasSetMode);
	p.Do(mode);
	p.Do(resumeMode);
	p.Do(holdMode);
	if (s >= 4) {
		p.Do(brightnessLevel);
	}
	p.Do(width);
	p.Do(height);
	WaitVBlankInfo wvi(0);
	p.Do(vblankWaitingThreads, wvi);
	p.Do(vblankPausedWaits);

	p.Do(enterVblankEvent);
	CoreTiming::RestoreRegisterEvent(enterVblankEvent, "EnterVBlank", &hleEnterVblank);
	p.Do(leaveVblankEvent);
	CoreTiming::RestoreRegisterEvent(leaveVblankEvent, "LeaveVBlank", &hleLeaveVblank);
	p.Do(afterFlipEvent);
	CoreTiming::RestoreRegisterEvent(afterFlipEvent, "AfterFlip", &hleAfterFlip);

	if (s >= 5) {
		p.Do(lagSyncEvent);
		p.Do(lagSyncScheduled);
		CoreTiming::RestoreRegisterEvent(lagSyncEvent, "LagSync", &hleLagSync);
		lastLagSync = real_time_now();
		if (lagSyncScheduled != g_Config.bForceLagSync) {
			ScheduleLagSync();
		}
	} else {
		lagSyncEvent = CoreTiming::RegisterEvent("LagSync", &hleLagSync);
		ScheduleLagSync();
	}

	p.Do(gstate);

	// TODO: GPU stuff is really not the responsibility of sceDisplay.
	// Display just displays the buffers the GPU has drawn, they are really completely distinct.
	// Maybe a bit tricky to move at this point, though...

	gstate_c.DoState(p);
#ifndef _XBOX
	if (s < 2) {
		// This shouldn't have been savestated anyway, but it was.
		// It's unlikely to overlap with the first value in gpuStats.
		p.ExpectVoid(&gl_extensions.gpuVendor, sizeof(gl_extensions.gpuVendor));
	}
#endif
	if (s < 6) {
		p.Do(gpuStats);

		// Removed values from gpuStats.
		int ignore = 42;
		p.Do(ignore);
		p.Do(ignore);
	}
	gpu->DoState(p);

	ReapplyGfxState();

	if (p.mode == p.MODE_READ) {
		if (hasSetMode) {
			gpu->InitClear();
		}
		gpu->SetDisplayFramebuffer(framebuf.topaddr, framebuf.pspFramebufLinesize, framebuf.pspFramebufFormat);
	}
}

void __DisplayShutdown() {
	vblankListeners.clear();
	vblankWaitingThreads.clear();
}

void __DisplayListenVblank(VblankCallback callback) {
	vblankListeners.push_back(callback);
}

static void __DisplayFireVblank() {
	for (std::vector<VblankCallback>::iterator iter = vblankListeners.begin(), end = vblankListeners.end(); iter != end; ++iter) {
		VblankCallback cb = *iter;
		cb();
	}
}

void __DisplayVblankBeginCallback(SceUID threadID, SceUID prevCallbackId) {
	SceUID pauseKey = prevCallbackId == 0 ? threadID : prevCallbackId;

	// This means two callbacks in a row.  PSP crashes if the same callback waits inside itself (may need more testing.)
	// TODO: Handle this better?
	if (vblankPausedWaits.find(pauseKey) != vblankPausedWaits.end()) {
		return;
	}

	WaitVBlankInfo waitData(0);
	for (size_t i = 0; i < vblankWaitingThreads.size(); i++) {
		WaitVBlankInfo *t = &vblankWaitingThreads[i];
		if (t->threadID == threadID) {
			waitData = *t;
			vblankWaitingThreads.erase(vblankWaitingThreads.begin() + i);
			break;
		}
	}

	if (waitData.threadID != threadID) {
		WARN_LOG_REPORT(SCEDISPLAY, "sceDisplayWaitVblankCB: could not find waiting thread info.");
		return;
	}

	vblankPausedWaits[pauseKey] = vCount + waitData.vcountUnblock;
	DEBUG_LOG(SCEDISPLAY, "sceDisplayWaitVblankCB: Suspending vblank wait for callback");
}

void __DisplayVblankEndCallback(SceUID threadID, SceUID prevCallbackId) {
	SceUID pauseKey = prevCallbackId == 0 ? threadID : prevCallbackId;

	// Probably should not be possible.
	if (vblankPausedWaits.find(pauseKey) == vblankPausedWaits.end()) {
		__KernelResumeThreadFromWait(threadID, 0);
		return;
	}

	int vcountUnblock = vblankPausedWaits[pauseKey];
	vblankPausedWaits.erase(pauseKey);
	if (vcountUnblock <= vCount) {
		__KernelResumeThreadFromWait(threadID, 0);
		return;
	}

	// Still have to wait a bit longer.
	vblankWaitingThreads.push_back(WaitVBlankInfo(__KernelGetCurThread(), vcountUnblock - vCount));
	DEBUG_LOG(SCEDISPLAY, "sceDisplayWaitVblankCB: Resuming vblank wait from callback");
}

// TODO: Also average actualFps
void __DisplayGetFPS(float *out_vps, float *out_fps, float *out_actual_fps) {
	*out_vps = fps;
	*out_fps = flips;
	*out_actual_fps = actualFps;
}

void __DisplayGetVPS(float *out_vps) {
	*out_vps = fps;
}

void __DisplayGetAveragedFPS(float *out_vps, float *out_fps) {
	float avg = 0.0;
	if (fpsHistoryValid > 0) {
		if (fpsHistoryValid > ARRAY_SIZE(fpsHistory)) {
			fpsHistoryValid = ARRAY_SIZE(fpsHistory);
		}
		for (size_t i = 0; i < fpsHistoryValid; ++i) {
			avg += fpsHistory[i];
		}
		avg /= (double) fpsHistoryValid;
	}

	*out_vps = *out_fps = avg;
}

static void CalculateFPS() {
	time_update();
	double now = time_now_d();

	if (now >= lastFpsTime + 1.0) {
		double frames = (gpuStats.numVBlanks - lastFpsFrame);
		actualFps = (actualFlips - lastActualFlips);

		fps = frames / (now - lastFpsTime);
		flips = 60.0 * (double) (gpuStats.numFlips - lastNumFlips) / frames;

		lastFpsFrame = gpuStats.numVBlanks;
		lastNumFlips = gpuStats.numFlips;
		lastActualFlips = actualFlips;
		lastFpsTime = now;

		fpsHistory[fpsHistoryPos++] = fps;
		fpsHistoryPos = fpsHistoryPos % ARRAY_SIZE(fpsHistory);
		++fpsHistoryValid;
	}
}

void __DisplayGetDebugStats(char stats[], size_t bufsize) {
	gpu->UpdateStats();

	float vertexAverageCycles = gpuStats.numVertsSubmitted > 0 ? (float)gpuStats.vertexGPUCycles / (float)gpuStats.numVertsSubmitted : 0.0f;

	snprintf(stats, bufsize - 1,
		"Frames: %i\n"
		"DL processing time: %0.2f ms\n"
		"Kernel processing time: %0.2f ms\n"
		"Slowest syscall: %s : %0.2f ms\n"
		"Most active syscall: %s : %0.2f ms\n"
		"Draw calls: %i, flushes %i\n"
		"Cached Draw calls: %i\n"
		"Num Tracked Vertex Arrays: %i\n"
		"Cycles executed: %d (%f per vertex)\n"
		"Commands per call level: %i %i %i %i\n"
		"Vertices Submitted: %i\n"
		"Cached Vertices Drawn: %i\n"
		"Uncached Vertices Drawn: %i\n"
		"FBOs active: %i\n"
		"Textures active: %i, decoded: %i\n"
		"Texture invalidations: %i\n"
		"Vertex shaders loaded: %i\n"
		"Fragment shaders loaded: %i\n"
		"Combined shaders loaded: %i\n",
		gpuStats.numVBlanks,
		gpuStats.msProcessingDisplayLists * 1000.0f,
		kernelStats.msInSyscalls * 1000.0f,
		kernelStats.slowestSyscallName ? kernelStats.slowestSyscallName : "(none)",
		kernelStats.slowestSyscallTime * 1000.0f,
		kernelStats.summedSlowestSyscallName ? kernelStats.summedSlowestSyscallName : "(none)",
		kernelStats.summedSlowestSyscallTime * 1000.0f,
		gpuStats.numDrawCalls,
		gpuStats.numFlushes,
		gpuStats.numCachedDrawCalls,
		gpuStats.numTrackedVertexArrays,
		gpuStats.vertexGPUCycles + gpuStats.otherGPUCycles,
		vertexAverageCycles,
		gpuStats.gpuCommandsAtCallLevel[0],gpuStats.gpuCommandsAtCallLevel[1],gpuStats.gpuCommandsAtCallLevel[2],gpuStats.gpuCommandsAtCallLevel[3],
		gpuStats.numVertsSubmitted,
		gpuStats.numCachedVertsDrawn,
		gpuStats.numUncachedVertsDrawn,
		gpuStats.numFBOs,
		gpuStats.numTextures,
		gpuStats.numTexturesDecoded,
		gpuStats.numTextureInvalidations,
		gpuStats.numVertexShaders,
		gpuStats.numFragmentShaders,
		gpuStats.numShaders
		);
	stats[bufsize - 1] = '\0';
	gpuStats.ResetFrame();
	kernelStats.ResetFrame();
}

enum {
	FPS_LIMIT_NORMAL = 0,
	FPS_LIMIT_CUSTOM = 1,
};

void __DisplaySetWasPaused() {
	wasPaused = true;
}

static bool FrameTimingThrottled() {
	if (PSP_CoreParameter().fpsLimit == FPS_LIMIT_CUSTOM && g_Config.iFpsLimit == 0) {
		return false;
	}
	return !PSP_CoreParameter().unthrottle;
}

// Let's collect all the throttling and frameskipping logic here.
static void DoFrameTiming(bool &throttle, bool &skipFrame, float timestep) {
	PROFILE_THIS_SCOPE("timing");
	int fpsLimiter = PSP_CoreParameter().fpsLimit;
	throttle = FrameTimingThrottled();
	skipFrame = false;

	// Check if the frameskipping code should be enabled. If neither throttling or frameskipping is on,
	// we have nothing to do here.
	bool doFrameSkip = g_Config.iFrameSkip != 0;

	if (!throttle && g_Config.bFrameSkipUnthrottle) {
		doFrameSkip = true;
		skipFrame = true;
		if (numSkippedFrames >= 7) {
			skipFrame = false;
		}
		return;
	}

	if (!throttle && !doFrameSkip)
		return;

	time_update();

	float scaledTimestep = timestep;
	if (fpsLimiter == FPS_LIMIT_CUSTOM && g_Config.iFpsLimit != 0) {
		scaledTimestep *= 60.0f / g_Config.iFpsLimit;
	}

	if (lastFrameTime == 0.0 || wasPaused) {
		nextFrameTime = time_now_d() + scaledTimestep;
		if (wasPaused)
			wasPaused = false;
	} else {
		// Advance lastFrameTime by a constant amount each frame,
		// but don't let it get too far behind as things can get very jumpy.
		const double maxFallBehindFrames = 5.5;

		nextFrameTime = std::max(lastFrameTime + scaledTimestep, time_now_d() - maxFallBehindFrames * scaledTimestep);
	}
	curFrameTime = time_now_d();

	// Auto-frameskip automatically if speed limit is set differently than the default.
	bool useAutoFrameskip = g_Config.bAutoFrameSkip && g_Config.iRenderingMode != FB_NON_BUFFERED_MODE;
	if (g_Config.bAutoFrameSkip || (g_Config.iFrameSkip == 0 && fpsLimiter == FPS_LIMIT_CUSTOM && g_Config.iFpsLimit > 60)) {
		// autoframeskip
		// Argh, we are falling behind! Let's skip a frame and see if we catch up.
		if (curFrameTime > nextFrameTime && doFrameSkip) {
			skipFrame = true;
		}
	} else if (g_Config.iFrameSkip >= 1)	{
		// fixed frameskip
		if (numSkippedFrames >= g_Config.iFrameSkip)
			skipFrame = false;
		else
			skipFrame = true;
	}

	if (curFrameTime < nextFrameTime && throttle) {
		// If time gap is huge just jump (somebody unthrottled)
		if (nextFrameTime - curFrameTime > 2*scaledTimestep) {
			nextFrameTime = curFrameTime;
		} else {
			// Wait until we've caught up.
			while (time_now_d() < nextFrameTime) {
#ifdef _WIN32
				sleep_ms(1); // Sleep for 1ms on this thread
#else
				const double left = nextFrameTime - curFrameTime;
				usleep((long)(left * 1000000));
#endif
				time_update();
			}
		}
		curFrameTime = time_now_d();
	}

	lastFrameTime = nextFrameTime;
}

static void DoFrameIdleTiming() {
	PROFILE_THIS_SCOPE("timing");
	if (!FrameTimingThrottled() || !g_Config.bEnableSound || wasPaused) {
		return;
	}

	time_update();

	double dist = time_now_d() - lastFrameTime;
	// Ignore if the distance is just crazy.  May mean wrap or pause.
	if (dist < 0.0 || dist >= 15 * timePerVblank) {
		return;
	}

	float scaledVblank = timePerVblank;
	if (PSP_CoreParameter().fpsLimit == FPS_LIMIT_CUSTOM) {
		// 0 is handled in FrameTimingThrottled().
		scaledVblank *= 60.0f / g_Config.iFpsLimit;
	}

	// If we have over at least a vblank of spare time, maintain at least 30fps in delay.
	// This prevents fast forward during loading screens.
	const double thresh = lastFrameTime + (numVBlanksSinceFlip - 1) * scaledVblank;
	if (numVBlanksSinceFlip >= 2 && time_now_d() < thresh) {
		// Give a little extra wiggle room in case the next vblank does more work.
		const double goal = lastFrameTime + numVBlanksSinceFlip * scaledVblank - 0.001;
		while (time_now_d() < goal) {
#ifdef _WIN32
			sleep_ms(1);
#else
			const double left = goal - time_now_d();
			usleep((long)(left * 1000000));
#endif
			time_update();
		}
	}
}


void hleEnterVblank(u64 userdata, int cyclesLate) {
	int vbCount = userdata;

	DEBUG_LOG(SCEDISPLAY, "Enter VBlank %i", vbCount);

	isVblank = 1;
	vCount++; // vCount increases at each VBLANK.
	hCountBase += hCountPerVblank; // This is the "accumulated" hcount base.
	if (hCountBase > 0x7FFFFFFF) {
		hCountBase -= 0x80000000;
	}
	frameStartTicks = CoreTiming::GetTicks();

	CoreTiming::ScheduleEvent(msToCycles(vblankMs) - cyclesLate, leaveVblankEvent, vbCount + 1);

	// Wake up threads waiting for VBlank
	u32 error;
	bool wokeThreads = false;
	for (size_t i = 0; i < vblankWaitingThreads.size(); i++) {
		if (--vblankWaitingThreads[i].vcountUnblock == 0) {
			// Only wake it if it wasn't already released by someone else.
			SceUID waitID = __KernelGetWaitID(vblankWaitingThreads[i].threadID, WAITTYPE_VBLANK, error);
			if (waitID == 1) {
				__KernelResumeThreadFromWait(vblankWaitingThreads[i].threadID, 0);
				wokeThreads = true;
			}
			vblankWaitingThreads.erase(vblankWaitingThreads.begin() + i--);
		}
	}
	if (wokeThreads) {
		__KernelReSchedule("entered vblank");
	}

	// Trigger VBlank interrupt handlers.
	__TriggerInterrupt(PSP_INTR_IMMEDIATE | PSP_INTR_ONLY_IF_ENABLED | PSP_INTR_ALWAYS_RESCHED, PSP_VBLANK_INTR, PSP_INTR_SUB_ALL);

	gpuStats.numVBlanks++;

	numVBlanksSinceFlip++;

	// TODO: Should this be done here or in hleLeaveVblank?
	if (framebufIsLatched) {
		DEBUG_LOG(SCEDISPLAY, "Setting latched framebuffer %08x (prev: %08x)", latchedFramebuf.topaddr, framebuf.topaddr);
		framebuf = latchedFramebuf;
		framebufIsLatched = false;
		gpu->SetDisplayFramebuffer(framebuf.topaddr, framebuf.pspFramebufLinesize, framebuf.pspFramebufFormat);
	}
	// We flip only if the framebuffer was dirty. This eliminates flicker when using
	// non-buffered rendering. The interaction with frame skipping seems to need
	// some work.
	// But, let's flip at least once every 10 frames if possible, since there may be sound effects.
	if (gpu->FramebufferDirty() || (g_Config.iRenderingMode != 0 && numVBlanksSinceFlip >= 10)) {
		if (g_Config.iShowFPSCounter && g_Config.iShowFPSCounter < 4) {
			CalculateFPS();
		}

		// Setting CORE_NEXTFRAME causes a swap.
		// Check first though, might've just quit / been paused.
		if (gpu->FramebufferReallyDirty()) {
			if (coreState == CORE_RUNNING) {
				coreState = CORE_NEXTFRAME;
				gpu->CopyDisplayToOutput();
				actualFlips++;
			}
		}

		gpuStats.numFlips++;

		bool throttle, skipFrame;
		DoFrameTiming(throttle, skipFrame, (float)numVBlanksSinceFlip * timePerVblank);

		int maxFrameskip = 8;
		if (throttle) {
			// 4 here means 1 drawn, 4 skipped - so 12 fps minimum.
			maxFrameskip = g_Config.iFrameSkip;
		}
		if (numSkippedFrames >= maxFrameskip) {
			skipFrame = false;
		}

		if (skipFrame) {
			gstate_c.skipDrawReason |= SKIPDRAW_SKIPFRAME;
			numSkippedFrames++;
		} else {
			gstate_c.skipDrawReason &= ~SKIPDRAW_SKIPFRAME;
			numSkippedFrames = 0;
		}

		// Returning here with coreState == CORE_NEXTFRAME causes a buffer flip to happen (next frame).
		// Right after, we regain control for a little bit in hleAfterFlip. I think that's a great
		// place to do housekeeping.
		CoreTiming::ScheduleEvent(0 - cyclesLate, afterFlipEvent, 0);
		numVBlanksSinceFlip = 0;
	} else {
		// Okay, there's no new frame to draw.  But audio may be playing, so we need to time still.
		DoFrameIdleTiming();
	}
}

void hleAfterFlip(u64 userdata, int cyclesLate) {
	gpu->BeginFrame();  // doesn't really matter if begin or end of frame.

	// This seems like as good a time as any to check if the config changed.
	if (lagSyncScheduled != g_Config.bForceLagSync) {
		ScheduleLagSync();
	}
}

void hleLeaveVblank(u64 userdata, int cyclesLate) {
	isVblank = 0;
	DEBUG_LOG(SCEDISPLAY,"Leave VBlank %i", (int)userdata - 1);
	CoreTiming::ScheduleEvent(msToCycles(frameMs - vblankMs) - cyclesLate, enterVblankEvent, userdata);

	// Fire the vblank listeners after the vblank completes.
	__DisplayFireVblank();
}

void hleLagSync(u64 userdata, int cyclesLate) {
	// The goal here is to prevent network, audio, and input lag from the real world.
	// Our normal timing is very "stop and go".  This is efficient, but causes real world lag.
	// This event (optionally) runs every 1ms to sync with the real world.
	PROFILE_THIS_SCOPE("timing");

	if (!FrameTimingThrottled()) {
		lagSyncScheduled = false;
		return;
	}

	float scale = 1.0f;
	if (PSP_CoreParameter().fpsLimit == FPS_LIMIT_CUSTOM) {
		// 0 is handled in FrameTimingThrottled().
		scale = 60.0f / g_Config.iFpsLimit;
	}

	const double goal = lastLagSync + (scale / 1000.0f);
	time_update();
	// Don't lag too long ever, if they leave it paused.
	while (time_now_d() < goal && goal < time_now_d() + 0.01) {
#ifndef _WIN32
		const double left = goal - time_now_d();
		usleep((long)(left * 1000000));
#endif
		time_update();
	}

	const int emuOver = (int)cyclesToUs(cyclesLate);
	const int over = (int)((time_now_d() - goal) * 1000000);
	ScheduleLagSync(over - emuOver);
}

static u32 sceDisplayIsVblank() {
	DEBUG_LOG(SCEDISPLAY,"%i=sceDisplayIsVblank()",isVblank);
	return isVblank;
}

static u32 sceDisplaySetMode(int displayMode, int displayWidth, int displayHeight) {
	if (displayWidth <= 0 || displayHeight <= 0 || (displayWidth & 0x7) != 0) {
		WARN_LOG(SCEDISPLAY, "sceDisplaySetMode INVALID SIZE (%i, %i, %i)", displayMode, displayWidth, displayHeight);
		return SCE_KERNEL_ERROR_INVALID_SIZE;
	}

	if (displayMode != PSP_DISPLAY_MODE_LCD) {
		WARN_LOG(SCEDISPLAY, "sceDisplaySetMode INVALID MODE(%i, %i, %i)", displayMode, displayWidth, displayHeight);
		return SCE_KERNEL_ERROR_INVALID_MODE;
	}

	DEBUG_LOG(SCEDISPLAY,"sceDisplaySetMode(%i, %i, %i)", displayMode, displayWidth, displayHeight);
	if (!hasSetMode) {
		gpu->InitClear();
		hasSetMode = true;
	}
	mode = displayMode;
	width = displayWidth;
	height = displayHeight;
	return 0;
}

// Some games (GTA) never call this during gameplay, so bad place to put a framerate counter.
static u32 sceDisplaySetFramebuf(u32 topaddr, int linesize, int pixelformat, int sync) {
	FrameBufferState fbstate = {0};
	if (topaddr != 0) {
		fbstate.topaddr = topaddr;
		fbstate.pspFramebufFormat = (GEBufferFormat)pixelformat;
		fbstate.pspFramebufLinesize = linesize;
	}

	hleEatCycles(290);

	s64 delayCycles = 0;
	if (topaddr != framebuf.topaddr && g_Config.iForceMaxEmulatedFPS > 0) {
		// Sometimes we get a small number, there's probably no need to delay the thread for this.
		// sceDisplaySetFramebuf() isn't supposed to delay threads at all.  This is a hack.
		const int FLIP_DELAY_CYCLES_MIN = 10;
		// Some games (like Final Fantasy 4) only call this too much in spurts.
		// The goal is to fix games where this would result in a consistent overhead.
		const int FLIP_DELAY_MIN_FLIPS = 30;

		u64 now = CoreTiming::GetTicks();
		// 1001 to account for NTSC timing (59.94 fps.)
		u64 expected = msToCycles(1001) / g_Config.iForceMaxEmulatedFPS;
		u64 actual = now - lastFlipCycles;
		if (actual < expected - FLIP_DELAY_CYCLES_MIN) {
			if (lastFlipsTooFrequent >= FLIP_DELAY_MIN_FLIPS) {
				delayCycles = expected - actual;
			} else {
				++lastFlipsTooFrequent;
			}
		} else {
			--lastFlipsTooFrequent;
		}
		lastFlipCycles = CoreTiming::GetTicks();
	}

	if (sync == PSP_DISPLAY_SETBUF_IMMEDIATE) {
		// Write immediately to the current framebuffer parameters
		framebuf = fbstate;
		gpu->SetDisplayFramebuffer(framebuf.topaddr, framebuf.pspFramebufLinesize, framebuf.pspFramebufFormat);
	} else {
		// Delay the write until vblank
		latchedFramebuf = fbstate;
		framebufIsLatched = true;
	}

	if (delayCycles > 0) {
		// Okay, the game is going at too high a frame rate.  God of War and Fat Princess both do this.
		// Simply eating the cycles works and is fast, but breaks other games (like Jeanne d'Arc.)
		// So, instead, we delay this HLE thread only (a small deviation from correct behavior.)
		return hleDelayResult(hleLogSuccessI(SCEDISPLAY, 0, "delaying frame thread"), "set framebuf", cyclesToUs(delayCycles));
	} else {
		if (topaddr == 0) {
			return hleLogSuccessI(SCEDISPLAY, 0, "disabling display");
		} else {
			return hleLogSuccessI(SCEDISPLAY, 0);
		}
	}
}

bool __DisplayGetFramebuf(u8 **topaddr, u32 *linesize, u32 *pixelFormat, int latchedMode) {
	const FrameBufferState &fbState = latchedMode == 1 ? latchedFramebuf : framebuf;
	if (topaddr != nullptr)
		*topaddr = Memory::GetPointer(fbState.topaddr);
	if (linesize != nullptr)
		*linesize = fbState.pspFramebufLinesize;
	if (pixelFormat != nullptr)
		*pixelFormat = fbState.pspFramebufFormat;

	return true;
}

static u32 sceDisplayGetFramebuf(u32 topaddrPtr, u32 linesizePtr, u32 pixelFormatPtr, int latchedMode) {
	const FrameBufferState &fbState = latchedMode == 1 && framebufIsLatched ? latchedFramebuf : framebuf;

	if (Memory::IsValidAddress(topaddrPtr))
		Memory::Write_U32(fbState.topaddr, topaddrPtr);
	if (Memory::IsValidAddress(linesizePtr))
		Memory::Write_U32(fbState.pspFramebufLinesize, linesizePtr);
	if (Memory::IsValidAddress(pixelFormatPtr))
		Memory::Write_U32(fbState.pspFramebufFormat, pixelFormatPtr);

	return hleLogSuccessI(SCEDISPLAY, 0);
}

static int DisplayWaitForVblanks(const char *reason, int vblanks, bool callbacks = false) {
	const s64 ticksIntoFrame = CoreTiming::GetTicks() - frameStartTicks;
	const s64 cyclesToNextVblank = msToCycles(frameMs) - ticksIntoFrame;

	// These syscalls take about 115 us, so if the next vblank is before then, we're waiting extra.
	// At least, on real firmware a wait >= 16500 into the frame will wait two.
	if (cyclesToNextVblank <= usToCycles(115)) {
		++vblanks;
	}

	vblankWaitingThreads.push_back(WaitVBlankInfo(__KernelGetCurThread(), vblanks));
	__KernelWaitCurThread(WAITTYPE_VBLANK, 1, 0, 0, callbacks, reason);

	return hleLogSuccessVerboseI(SCEDISPLAY, 0, "waiting for %d vblanks", vblanks);
}

static int DisplayWaitForVblanksCB(const char *reason, int vblanks) {
	return DisplayWaitForVblanks(reason, vblanks, true);
}

static u32 sceDisplayWaitVblankStart() {
	return DisplayWaitForVblanks("vblank start waited", 1);
}

static u32 sceDisplayWaitVblank() {
	if (!isVblank) {
		return DisplayWaitForVblanks("vblank waited", 1);
	} else {
		hleEatCycles(1110);
		hleReSchedule("vblank wait skipped");
		return hleLogSuccessI(SCEDISPLAY, 1, "not waiting since in vblank");
	}
}

static u32 sceDisplayWaitVblankStartMulti(int vblanks) {
	if (vblanks <= 0) {
		return hleLogWarning(SCEDISPLAY, SCE_KERNEL_ERROR_INVALID_VALUE, "invalid number of vblanks");
	}
	if (!__KernelIsDispatchEnabled())
		return hleLogWarning(SCEDISPLAY, SCE_KERNEL_ERROR_CAN_NOT_WAIT, "dispatch disabled");
	if (__IsInInterrupt())
		return hleLogWarning(SCEDISPLAY, SCE_KERNEL_ERROR_ILLEGAL_CONTEXT, "in interrupt");

	return DisplayWaitForVblanks("vblank start multi waited", vblanks);
}

static u32 sceDisplayWaitVblankCB() {
	if (!isVblank) {
		return DisplayWaitForVblanksCB("vblank waited", 1);
	} else {
		hleEatCycles(1110);
		hleReSchedule("vblank wait skipped");
		return hleLogSuccessI(SCEDISPLAY, 1, "not waiting since in vblank");
	}
}

static u32 sceDisplayWaitVblankStartCB() {
	return DisplayWaitForVblanksCB("vblank start waited", 1);
}

static u32 sceDisplayWaitVblankStartMultiCB(int vblanks) {
	if (vblanks <= 0) {
		return hleLogWarning(SCEDISPLAY, SCE_KERNEL_ERROR_INVALID_VALUE, "invalid number of vblanks");
	}
	if (!__KernelIsDispatchEnabled())
		return hleLogWarning(SCEDISPLAY, SCE_KERNEL_ERROR_CAN_NOT_WAIT, "dispatch disabled");
	if (__IsInInterrupt())
		return hleLogWarning(SCEDISPLAY, SCE_KERNEL_ERROR_ILLEGAL_CONTEXT, "in interrupt");

	return DisplayWaitForVblanksCB("vblank start multi waited", vblanks);
}

static u32 sceDisplayGetVcount() {
	hleEatCycles(150);
	hleReSchedule("get vcount");
	return hleLogSuccessVerboseI(SCEDISPLAY, vCount);
}

static u32 __DisplayGetCurrentHcount() {
	const static int ticksPerVblank333 = 333 * 1000000 / 60 / hCountPerVblank;
	const int ticksIntoFrame = CoreTiming::GetTicks() - frameStartTicks;
	// Can't seem to produce a 0 on real hardware, offsetting by 1 makes things look right.
	return 1 + (ticksIntoFrame / (CoreTiming::GetClockFrequencyMHz() * ticksPerVblank333 / 333));
}

static u32 __DisplayGetAccumulatedHcount() {
	// The hCount is always a positive int, and wraps from 0x7FFFFFFF -> 0.
	int value = hCountBase + __DisplayGetCurrentHcount();
	return value & 0x7FFFFFFF;
}

static u32 sceDisplayGetCurrentHcount() {
	hleEatCycles(275);
	return hleLogSuccessI(SCEDISPLAY, __DisplayGetCurrentHcount());
}

static int sceDisplayAdjustAccumulatedHcount(int value) {
	if (value < 0) {
		return hleLogError(SCEDISPLAY, SCE_KERNEL_ERROR_INVALID_VALUE, "invalid value");
	}

	// Since it includes the current hCount, find the difference to apply to the base.
	u32 accumHCount = __DisplayGetAccumulatedHcount();
	int diff = value - accumHCount;
	hCountBase += diff;

	return hleLogSuccessI(SCEDISPLAY, 0);
}

static int sceDisplayGetAccumulatedHcount() {
	u32 accumHCount = __DisplayGetAccumulatedHcount();
	DEBUG_LOG(SCEDISPLAY, "%d=sceDisplayGetAccumulatedHcount()", accumHCount);
	hleEatCycles(235);
	return accumHCount;
}

static float sceDisplayGetFramePerSec() {
	const static float framePerSec = 59.9400599f;
	DEBUG_LOG(SCEDISPLAY,"%f=sceDisplayGetFramePerSec()", framePerSec);
	return framePerSec;	// (9MHz * 1)/(525 * 286)
}

static u32 sceDisplayIsForeground() {
	DEBUG_LOG(SCEDISPLAY,"IMPL sceDisplayIsForeground()");
	if (!hasSetMode || framebuf.topaddr == 0)
		return 0;
	else
		return 1;   // return value according to JPCSP comment
}

static u32 sceDisplayGetMode(u32 modeAddr, u32 widthAddr, u32 heightAddr) {
	DEBUG_LOG(SCEDISPLAY,"sceDisplayGetMode(%08x, %08x, %08x)", modeAddr, widthAddr, heightAddr);
	if (Memory::IsValidAddress(modeAddr))
		Memory::Write_U32(mode, modeAddr);
	if (Memory::IsValidAddress(widthAddr))
		Memory::Write_U32(width, widthAddr);
	if (Memory::IsValidAddress(heightAddr))
		Memory::Write_U32(height, heightAddr);
	return 0;
}

static u32 sceDisplayIsVsync() {
	ERROR_LOG(SCEDISPLAY,"UNIMPL sceDisplayIsVsync()");
	return 0;
}

static u32 sceDisplayGetResumeMode(u32 resumeModeAddr) {
	ERROR_LOG(SCEDISPLAY,"sceDisplayGetResumeMode(%08x)", resumeModeAddr);
	if (Memory::IsValidAddress(resumeModeAddr))
		Memory::Write_U32(resumeMode, resumeModeAddr);
	return 0;
}

static u32 sceDisplaySetResumeMode(u32 rMode) {
	ERROR_LOG(SCEDISPLAY,"sceDisplaySetResumeMode(%08x)", rMode);
	resumeMode = rMode;
	return 0;
}

static u32 sceDisplayGetBrightness(u32 levelAddr) {
	ERROR_LOG(SCEDISPLAY,"sceDisplayGetBrightness(%08x)", levelAddr);
	if (Memory::IsValidAddress(levelAddr))
		Memory::Write_U32(brightnessLevel, levelAddr);
	return 0;
}

static u32 sceDisplaySetBrightness(u32 bLevel) {
	ERROR_LOG(SCEDISPLAY,"sceDisplaySetBrightness(%08x)", bLevel);
	brightnessLevel = bLevel;
	return 0;
}

static u32 sceDisplaySetHoldMode(u32 hMode) {
	ERROR_LOG(SCEDISPLAY,"sceDisplaySetHoldMode(%08x)", hMode);
	holdMode = hMode;
	return 0;
}

const HLEFunction sceDisplay[] = {
	{0X0E20F177, &WrapU_III<sceDisplaySetMode>,               "sceDisplaySetMode",                 'x', "iii" },
	{0X289D82FE, &WrapU_UIII<sceDisplaySetFramebuf>,          "sceDisplaySetFrameBuf",             'x', "xiii"},
	{0XEEDA2E54, &WrapU_UUUI<sceDisplayGetFramebuf>,          "sceDisplayGetFrameBuf",             'x', "pppi"},
	{0X36CDFADE, &WrapU_V<sceDisplayWaitVblank>,              "sceDisplayWaitVblank",              'x', "",   HLE_NOT_DISPATCH_SUSPENDED },
	{0X984C27E7, &WrapU_V<sceDisplayWaitVblankStart>,         "sceDisplayWaitVblankStart",         'x', "",   HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0X40F1469C, &WrapU_I<sceDisplayWaitVblankStartMulti>,    "sceDisplayWaitVblankStartMulti",    'x', "i"   },
	{0X8EB9EC49, &WrapU_V<sceDisplayWaitVblankCB>,            "sceDisplayWaitVblankCB",            'x', "",   HLE_NOT_DISPATCH_SUSPENDED },
	{0X46F186C3, &WrapU_V<sceDisplayWaitVblankStartCB>,       "sceDisplayWaitVblankStartCB",       'x', "",   HLE_NOT_IN_INTERRUPT | HLE_NOT_DISPATCH_SUSPENDED },
	{0X77ED8B3A, &WrapU_I<sceDisplayWaitVblankStartMultiCB>,  "sceDisplayWaitVblankStartMultiCB",  'x', "i"   },
	{0XDBA6C4C4, &WrapF_V<sceDisplayGetFramePerSec>,          "sceDisplayGetFramePerSec",          'f', ""    },
	{0X773DD3A3, &WrapU_V<sceDisplayGetCurrentHcount>,        "sceDisplayGetCurrentHcount",        'x', ""    },
	{0X210EAB3A, &WrapI_V<sceDisplayGetAccumulatedHcount>,    "sceDisplayGetAccumulatedHcount",    'i', ""    },
	{0XA83EF139, &WrapI_I<sceDisplayAdjustAccumulatedHcount>, "sceDisplayAdjustAccumulatedHcount", 'i', "i"   },
	{0X9C6EAAD7, &WrapU_V<sceDisplayGetVcount>,               "sceDisplayGetVcount",               'x', ""    },
	{0XDEA197D4, &WrapU_UUU<sceDisplayGetMode>,               "sceDisplayGetMode",                 'x', "xxx" },
	{0X7ED59BC4, &WrapU_U<sceDisplaySetHoldMode>,             "sceDisplaySetHoldMode",             'x', "x"   },
	{0XA544C486, &WrapU_U<sceDisplaySetResumeMode>,           "sceDisplaySetResumeMode",           'x', "x"   },
	{0XBF79F646, &WrapU_U<sceDisplayGetResumeMode>,           "sceDisplayGetResumeMode",           'x', "x"   },
	{0XB4F378FA, &WrapU_V<sceDisplayIsForeground>,            "sceDisplayIsForeground",            'x', ""    },
	{0X31C4BAA8, &WrapU_U<sceDisplayGetBrightness>,           "sceDisplayGetBrightness",           'x', "x"   },
	{0X9E3C6DC6, &WrapU_U<sceDisplaySetBrightness>,           "sceDisplaySetBrightness",           'x', "x"   },
	{0X4D4E10EC, &WrapU_V<sceDisplayIsVblank>,                "sceDisplayIsVblank",                'x', ""    },
	{0X21038913, &WrapU_V<sceDisplayIsVsync>,                 "sceDisplayIsVsync",                 'x', ""    },
};

void Register_sceDisplay() {
	RegisterModule("sceDisplay", ARRAY_SIZE(sceDisplay), sceDisplay);
}

void Register_sceDisplay_driver() {
	RegisterModule("sceDisplay_driver", ARRAY_SIZE(sceDisplay), sceDisplay);
}
