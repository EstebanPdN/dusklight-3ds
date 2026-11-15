/*
libctru v2.7.0 GPU command queue, devkitPro contributors.
Original: https://github.com/devkitPro/libctru/blob/v2.7.0/libctru/source/gpu/gxqueue.c
Original body SHA-256: 9862fef36f458afefac753a31808f92eac5e59de9562c8145a4a9af0469535d5
Local adaptation: CMake renames the IRQ entry to duskOriginalGxCmdQueueInterrupt
so gpu_sync_ctru.cpp can serialize the public entry through its original
callback. No SDK callback is replaced. Two DuskLight changes (2026-11-13), made
after three New 3DS runs hung where Azahar did not:
  1. One job in flight (upstream: 3). The engine queues many command lists per
     frame; upstream Citro3D queues one. Azahar executes each job at once and
     in order, so this is the model the engine was validated against.
  2. A job completes only on its own event (P3D, PPF, PSC0/PSC1, DMA), not on
     any interrupt, so a stray or missing event cannot shift the count.
  3. (2026-11-15) A ring of the last 32 jobs: the words sent to GSP, when each
     was submitted and when its event came. The GPU guard writes it into the
     hang record and the dump: Alpha 2 stopped on a transfer whose words were
     not in any record.

This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any
  damages arising from the use of this software.

  Permission is granted to anyone to use this software for any
  purpose, including commercial applications, and to alter it and
  redistribute it freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you
     must not claim that you wrote the original software. If you use
     this software in a product, an acknowledgment in the product
     documentation would be appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and
     must not be misrepresented as being the original software.
  3. This notice may not be removed or altered from any source
     distribution.
*/
#include <stdlib.h>
#include <string.h>
#include <3ds/types.h>
#include <3ds/svc.h>
#include <3ds/synchronization.h>
#include <3ds/gpu/gx.h>
#include <3ds/services/gspgpu.h>

#define MAX_PARALLEL_CMDS 1

// Event raised by the GPU when a queued job finishes; -1 when unknown.
int duskGxCmdQueueExpectedEvent(const gxCmdEntry_s* entry)
{
	switch (entry->type)
	{
		case 0x00: return GSPGPU_EVENT_DMA;
		case 0x01: return GSPGPU_EVENT_P3D;
		case 0x02: return entry->args[0] ? GSPGPU_EVENT_PSC0 : GSPGPU_EVENT_PSC1;
		case 0x03: case 0x04: return GSPGPU_EVENT_PPF;
		default: return -1;
	}
}

static gxCmdQueue_s* curQueue;
static bool isActive, isRunning, shouldStop;
static LightLock queueLock = 1;

// Last jobs sent to GSP (record n of ring slot n % 32). Written under
// queueLock by the submitting thread and by the GSP event thread; read without
// the lock by the guard and the dump (diagnostics only).
typedef struct { u32 data[8]; u64 submitted, completed; u32 event, assumed; } duskGxJobRecord;
#define DUSK_GX_JOB_RING 32
static duskGxJobRecord duskJobRing[DUSK_GX_JOB_RING];
static u32 duskJobsSubmitted, duskJobsCompleted;
static void duskJobSubmitted(const u32* data)
{
	duskGxJobRecord* record = &duskJobRing[duskJobsSubmitted % DUSK_GX_JOB_RING];
	memcpy(record->data, data, sizeof(record->data));
	record->submitted = svcGetSystemTick();
	record->completed = 0;
	record->event = ~0u;
	record->assumed = 0;
	duskJobsSubmitted++;
}
static void duskJobCompleted(GSPGPU_Event irq)
{
	if (duskJobsCompleted >= duskJobsSubmitted) return;
	duskGxJobRecord* record = &duskJobRing[duskJobsCompleted % DUSK_GX_JOB_RING];
	record->completed = svcGetSystemTick();
	record->event = (u32)irq;
	duskJobsCompleted++;
}
// The guard counted a silent transfer complete: mark the last completed job.
void duskGxJobMarkAssumed(void)
{
	if (duskJobsCompleted)
		duskJobRing[(duskJobsCompleted - 1) % DUSK_GX_JOB_RING].assumed = 1;
}
// Copies up to max records, oldest first; returns how many. out holds
// 8 data words, submitted tick (2 words), completed tick (2 words), event,
// assumed: 14 words per record.
unsigned duskGxJobRingCopy(u32* out, unsigned max, u32* submittedTotal)
{
	const u32 total = duskJobsSubmitted;
	unsigned count = total < DUSK_GX_JOB_RING ? total : DUSK_GX_JOB_RING;
	if (count > max) count = max;
	for (unsigned i = 0; i < count; ++i)
	{
		const duskGxJobRecord* r = &duskJobRing[(total - count + i) % DUSK_GX_JOB_RING];
		u32* o = out + i * 14;
		memcpy(o, r->data, 32);
		o[8] = (u32)r->submitted; o[9] = (u32)(r->submitted >> 32);
		o[10] = (u32)r->completed; o[11] = (u32)(r->completed >> 32);
		o[12] = r->event; o[13] = r->assumed;
	}
	if (submittedTotal) *submittedTotal = total;
	return count;
}

static void gxCmdQueueDoCommands(void)
{
	if (shouldStop)
		return;
	int batchSize = curQueue->lastEntry+MAX_PARALLEL_CMDS-curQueue->curEntry;
	while (curQueue->curEntry < curQueue->numEntries && batchSize--)
	{
		gxCmdEntry_s* entry = &curQueue->entries[curQueue->curEntry++];
		duskJobSubmitted(entry->data);
		gspSubmitGxCommand(entry->data);
	}
}

void gxCmdQueueInterrupt(GSPGPU_Event irq)
{
	if (!isRunning || irq==GSPGPU_EVENT_VBlank0 || irq==GSPGPU_EVENT_VBlank1)
		return;
	gxCmdQueue_s* runCb = NULL;
	LightLock_Lock(&queueLock);
	int expected = curQueue && curQueue->lastEntry < curQueue->curEntry ?
		duskGxCmdQueueExpectedEvent(&curQueue->entries[curQueue->lastEntry]) : -2;
	if (expected == -2 || (expected >= 0 ? (int)irq != expected : irq == GSPGPU_EVENT_PSC1))
	{
		// Not the event of the job in flight (e.g. PSC1 of a two-buffer fill).
		LightLock_Unlock(&queueLock);
		return;
	}
	curQueue->lastEntry++;
	duskJobCompleted(irq);
	if (shouldStop)
	{
		curQueue = NULL;
		isActive = false;
		isRunning = false;
		shouldStop = false;
	}
	else if (curQueue->lastEntry < curQueue->numEntries)
		gxCmdQueueDoCommands();
	else
	{
		runCb = curQueue;
		isRunning = false;
	}
	LightLock_Unlock(&queueLock);
	if (runCb && runCb->callback)
		runCb->callback(runCb);
}

void gxCmdQueueClear(gxCmdQueue_s* queue)
{
	if (queue==curQueue && isRunning)
		svcBreak(USERBREAK_PANIC); // Shouldn't happen.
	queue->numEntries = 0;
	queue->curEntry = 0;
	queue->lastEntry = 0;
}

void gxCmdQueueAdd(gxCmdQueue_s* queue, const gxCmdEntry_s* entry)
{
	if (queue->numEntries == queue->maxEntries)
		svcBreak(USERBREAK_PANIC); // Shouldn't happen.
	memcpy(&queue->entries[queue->numEntries], entry, sizeof(gxCmdEntry_s));
	LightLock_Lock(&queueLock);
	queue->numEntries++;
	if (queue==curQueue && isActive && !isRunning)
	{
		isRunning = true;
		gxCmdQueueDoCommands();
	}
	LightLock_Unlock(&queueLock);
}

void gxCmdQueueRun(gxCmdQueue_s* queue)
{
	if (isRunning)
		return;
	curQueue = queue;
	isActive = true;
	if (queue->lastEntry < queue->numEntries)
	{
		isRunning = true;
		LightLock_Lock(&queueLock);
		gxCmdQueueDoCommands();
		LightLock_Unlock(&queueLock);
	} else
		isRunning = false;
}

void gxCmdQueueStop(gxCmdQueue_s* queue)
{
	if (!curQueue)
		return;
	LightLock_Lock(&queueLock);
	if (!isRunning)
	{
		curQueue = NULL;
		isActive = false;
	} else
		shouldStop = true;
	LightLock_Unlock(&queueLock);
}

bool gxCmdQueueWait(gxCmdQueue_s* queue, s64 timeout)
{
	u64 deadline = U64_MAX;
	if (timeout >= 0)
		deadline = svcGetSystemTick() + timeout;
	while (isRunning)
	{
		if (timeout >= 0 && (s64)(u64)(svcGetSystemTick()-deadline) >= 0)
			return false;
		gspWaitForAnyEvent();
	}
	return true;
}
