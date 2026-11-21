#ifndef F_PC_ID_INDEX_H_
#define F_PC_ID_INDEX_H_

// Native New 3DS (logic.txt procindex): the processes reachable by
// fpcLyIt_AllJudge (every layer of the layer list, every list of its node
// tree), by id, so fpcEx_SearchByID does not walk them all. f_pc_layer.cpp
// changes the table exactly where a layer tag enters or leaves a node tree
// (fpcLy_IntoQueue / fpcLy_ToQueue / fpcLy_QueueTo on success) and stops it
// when fpcLy_Delete would leave processes unreachable. Ids come from
// fpcBs_MakeOfId (one counter, never reused). Anything unexpected (a
// duplicate id, the removal of an id it does not hold, a full table) stops
// the table for the rest of the session: searches walk the layers again.
//
// Self-contained (no game headers) so the host test drives this code.

template <typename Proc, typename Id>
struct fpcIdIndexTable {
    enum { kSlots = 4096, kMask = kSlots - 1, kMaxLive = 3072 };
    struct Slot {
        Id id;
        Proc* proc;
    };
    Slot slots[kSlots];
    unsigned live;
    unsigned peak;
    // 0 = valid; else why it stopped: 1 tag without a process, 2 full,
    // 3 duplicate id, 4 removal of an id not held, 5 removal of another
    // process with the id, 6 deleted layer still holding processes.
    unsigned stopped;

    static unsigned home(Id id) { return unsigned(id) & kMask; }

    void add(Proc* proc) {
        if (stopped != 0) {
            return;
        }
        if (proc == nullptr) {
            stopped = 1;
            return;
        }
        if (live >= kMaxLive) {
            stopped = 2;
            return;
        }
        const Id id = proc->id;
        unsigned i = home(id);
        while (slots[i].proc != nullptr) {
            if (slots[i].id == id) {
                stopped = 3;
                return;
            }
            i = (i + 1) & kMask;
        }
        slots[i].id = id;
        slots[i].proc = proc;
        if (++live > peak) {
            peak = live;
        }
    }

    void remove(Proc* proc) {
        if (stopped != 0) {
            return;
        }
        if (proc == nullptr) {
            stopped = 1;
            return;
        }
        const Id id = proc->id;
        unsigned i = home(id);
        for (;;) {
            if (slots[i].proc == nullptr) {
                stopped = 4;
                return;
            }
            if (slots[i].id == id) {
                break;
            }
            i = (i + 1) & kMask;
        }
        if (slots[i].proc != proc) {
            stopped = 5;
            return;
        }
        // Backward-shift deletion: no tombstones, probe runs stay short.
        unsigned j = i;
        for (;;) {
            j = (j + 1) & kMask;
            if (slots[j].proc == nullptr) {
                break;
            }
            const unsigned k = home(slots[j].id);
            const bool stays = i <= j ? (i < k && k <= j) : (i < k || k <= j);
            if (stays) {
                continue;
            }
            slots[i] = slots[j];
            i = j;
        }
        slots[i].id = Id(0);
        slots[i].proc = nullptr;
        live--;
    }

    // 1 and the process (null when none reachable has the id) when the table
    // answers; 0 when it has stopped.
    int find(Id id, Proc** out) const {
        if (stopped != 0) {
            return 0;
        }
        unsigned i = home(id);
        for (;;) {
            Proc* proc = slots[i].proc;
            if (proc == nullptr) {
                *out = nullptr;
                return 1;
            }
            if (slots[i].id == id) {
                *out = proc;
                return 1;
            }
            i = (i + 1) & kMask;
        }
    }
};

#if defined(DUSK_TARGET_3DS) && defined(F_PC_BASE_H_)
int fpcIdIdx_Find(fpc_ProcID i_id, base_process_class** o_proc);
void fpcIdIdx_State(unsigned* o_live, unsigned* o_peak, unsigned* o_stopped);
#endif

#endif
