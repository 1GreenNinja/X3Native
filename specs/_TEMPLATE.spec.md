# Spec: <Module Name>  (<Dn>)

> Written by the SPEC TEAM (14900K). Implemented by the CLEAN-ROOM TEAM (13700K) from THIS FILE + public refs ONLY.
> ❌ No GPL source, no transcribed function bodies, no RBDOOM identifiers/paths below this line.

- **Ledger ID:** D#
- **Implements interface:** `IXxx` (`engine/xxx/IXxx.h`)
- **Status:** SPEC | WIP | VERIFY | DONE-CLEAN
- **Spec author machine:** 14900K
- **Clean-room target machine:** 13700K

## 1. Purpose (one paragraph, prose)
What this subsystem does, in plain language. The *job*, not the implementation.

## 2. Interface contract
The exact API the clean impl must satisfy. (This interface is OUR clean design — define it without reference to RBDOOM's class shape.)

```cpp
// engine/xxx/IXxx.h  — clean, authored fresh
class IXxx {
public:
    virtual ~IXxx() = default;
    // virtual ReturnType method(Args) = 0;
};
```

## 3. Behavior
- Inputs: <what comes in, types, units, valid ranges>
- Outputs: <what goes out>
- Lifecycle: <init order, per-frame calls, teardown>
- Threading: <main-thread only? job-safe? which calls?>
- Invariants: <what must always hold>

## 4. Edge cases & error handling
- <empty input, resource exhaustion, device loss, etc. — expected behavior for each>

## 5. Performance targets
- <e.g., "≤ 0.5 ms/frame for N draws at 1080p on RTX 3060"; memory budget; allocation rules (no per-frame heap alloc in hot path)>

## 6. Acceptance tests (clean-room team implements these)
Each test = inputs → expected observable output. No reference to internal structure.
1. **T1 — <name>:** given <input>, expect <output>.
2. **T2 — <name>:** given <input>, expect <output>.
3. ...

## 7. Public references (cite ONLY public sources)
- <Vulkan spec section / RTR4 chapter / GPU Gems article / library docs URL>
- <...>

## 8. Suggested permissive libraries (clean IP, shippable closed-source)
- <e.g., vk-bootstrap (MIT), VMA (MIT), miniz (MIT), cgltf (MIT), Jolt (MIT)>

## 9. Notes for the clean-room implementer
- <hints expressed as goals, not as "do what RBDOOM does at line X">
