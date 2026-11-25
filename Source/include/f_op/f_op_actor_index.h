#ifndef F_OP_ACTOR_INDEX_H_
#define F_OP_ACTOR_INDEX_H_

// Native New 3DS (logic.txt actorindex): searches of the actor list
// (g_fopAcTg_Queue, fopAcIt_Judge) by id and by profile name without walking
// it. f_op_actor_tag.cpp keeps both tables where an actor tag enters the list
// (fopAcTg_ToActorQ, always appended at the tail: cLs_Addition) and where it
// leaves it (fopAcTg_ActorQTo, before the cut).
//
// By id: fpcIdIndexTable (f_pc/f_pc_id_index.h), one entry per actor in the
// list; ids are unique (fpcBs_MakeOfId).
// By name: for every profile name, the first actor of that name in list
// order, which is what fpcSch_JudgeForPName finds first. An appended actor
// is later than every actor already listed, so it only fills an empty entry;
// when the first actor of a name leaves, the next actor of that name after it
// in the list (a walk forward from its own node, done only then) takes its
// place.
//
// Self-contained (no game headers) so the host test drives this code.

template <typename Proc>
struct fopAcNameFirstTable {
    enum { kNames = 1024 };
    Proc* first[kNames];

    static bool indexed(int name) { return name >= 0 && name < kNames; }

    void added(Proc* proc) {
        const int name = proc->name;
        if (indexed(name) && first[name] == nullptr) {
            first[name] = proc;
        }
    }

    // Before the actor's node leaves the list: nextOf(node) is the following
    // node or null, procOf(node) the actor a node holds.
    template <typename Node, typename NextOf, typename ProcOf>
    void removing(Proc* proc, Node* node, NextOf nextOf, ProcOf procOf) {
        const int name = proc->name;
        if (!indexed(name) || first[name] != proc) {
            return;
        }
        Proc* found = nullptr;
        for (Node* next = nextOf(node); next != nullptr; next = nextOf(next)) {
            Proc* other = procOf(next);
            if (other->name == name) {
                found = other;
                break;
            }
        }
        first[name] = found;
    }

    // 1 and the first actor of the name (null if none); 0 when the name is
    // outside the table (the caller walks).
    int find(int name, Proc** out) const {
        if (!indexed(name)) {
            return 0;
        }
        *out = first[name];
        return 1;
    }
};

#endif
