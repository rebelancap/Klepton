// Managed-side probe: ask Unity what it thinks, in Unity's own words.
//
// Every input measurement so far has been taken on the *native* side of the
// boundary — what ovrp handed libunity, what libunity's joystick fill wrote.
// That leaves the last hop unmeasured: whether any of it survives into
// `UnityEngine.Input`, which is where this title actually reads controllers
//  (the InputManager binds TriggerLeftHand/TriggerRightHand to joystick axes 8 and 9). "Where Unity learns how many joysticks exist" was
// the named gap; `Input.GetJoystickNames()` answers it directly.
//
// libil2cpp exports the standard embedding API, so no address table or
// hand-built calling convention is needed: domain -> assembly -> image ->
// class -> MethodInfo -> il2cpp_runtime_invoke. The runtime does class init,
// argument marshalling and boxing itself, which is why this is the robust
// way in rather than calling compiled method bodies directly.
//
// Diagnostic only, and off unless KL_PROBE_INPUT is set.
#ifndef KL_MPROBE_H
#define KL_MPROBE_H

// Called from the frame pump with the frame index. Prints a line every
// KL_PROBE_EVERY frames (default 120). No-op unless KL_PROBE_INPUT=1.
void kl_mprobe_tick(unsigned frame);

// The same idea pointed at a scripted sequence instead of at input: read a
// live object's FIELDS rather than call its methods, so a state machine that
// has stopped advancing can be asked where it stopped. Disassembly can list
// the steps a coroutine queues; only the running object knows which one it is
// sitting on. Prints every KL_PROBE_TIMELINE frames; off when that is unset.
//
// KL_PROBE_CLASSREF=<hex>[,<hex>...] is the companion: an il2cpp class-ref
// slot (an image offset, read out of a disassembled `adrp/ldr` pair) resolved
// to a type NAME plus that type's static fields and their values. Compiled
// il2cpp code names nothing — a gate that early-outs an entire Update is an
// anonymous byte at `statics+0x14` until something reads the slot at runtime.
void kl_mprobe_timeline(unsigned frame);

// KL_PROBE_ENUM="Namespace.Type[/Nested][,...]" — print a managed enum's names
// and VALUES, read out of the running guest by reflection. Callable as soon as
// the IL2CPP runtime is up, which is the point: the values are what
// kl_ovrplat's message types have to be, and a guest that stops before the
// frame pump on the host (Arkham Shadow does) still has to be askable. The
// frame pump calls it too, once, on its first probing tick.
void kl_mprobe_enums(void);

#endif
