# Workspace directories

- This directory (`D:\work\stanleyljl\cocos-engine`) is the engine directory.
- `D:\work\editors\projects\landscape` is the project directory.
- `D:\work\jobs\terrain` contains documentation and demo files.

# Collaboration rules

- Preserve existing working features and behavior during all changes and refactors. Simplification is not permission to remove, disable, or degrade functionality (including Landscape morph transitions). Identify the affected behavior before editing and review its preservation afterward; compilation success alone does not establish behavioral equivalence. If a behavior change is necessary, explain it and obtain the user's explicit approval before implementing it.
- Required engine singletons and Landscape runtime dependencies must be initialized successfully before runtime work begins. Complete initialization before publishing runtime objects or applying runtime settings; initialization failure must prevent Landscape from running. Do not add runtime null guards, conditional calls, or default-value fallbacks for required singletons (such as Root and its device) or required submodules (such as VTRenderer, DecalRenderer, TilePagePool and LandscapeSchedule). Keep checks for genuinely optional resources, external inputs, resource creation failures and partial-initialization cleanup; do not treat required dependencies as optional.
- Run performance tests, profiling, and benchmarks only with Release builds. Do not use Debug builds for performance measurements or optimization conclusions; Debug builds may still be used for functional debugging and compilation checks.
- By default, validate only Windows Release builds. Validate Android only when the user explicitly requests it.
- Engine code maintainability is a hard requirement for every change, including performance optimizations. Follow established engine flows and keep module responsibilities clear. Avoid one-off special cases, cross-module interfaces, or extra state for local optimizations unless their necessity is demonstrated and the maintenance cost is justified. When a fix requires substantial complexity, explain the evidence and tradeoffs before implementing it.
- Before modifying shared/common modules, explain the reason, scope, and impact to the user and wait for their explicit decision. Do not extend a local feature or optimization into shared modules without approval.
- Only modify code and ensure it compiles.
- Do not automatically commit or push code unless the user explicitly requests it.
- Prefer SSH when connecting to remote Git repositories. For HTTPS-configured remotes, use a per-command SSH URL rewrite without changing the remote configuration.
- When adding native `.cpp` or `.h` files, register them in the owning CMake target/source list and regenerate the Visual Studio project so they appear in the project and filters. Do not rely only on an include directive or edit generated project files as the lasting fix.
- Keep small, closely related helpers in existing files when practical instead of introducing a separate file.
- Always put each statement on its own line. Never combine multiple statements on one line, including in short functions, branches, or lambdas.
- Declare each class or struct data member separately on its own line. Never combine multiple members in one declaration, including members of nested or implementation-local structs.
- Keep Landscape global function and member function signatures on one line in both declarations and definitions; do not wrap parameter lists.
- Whenever code changes, review the related comments and update or remove any outdated comments in the same change so they accurately describe the current implementation.
- Landscape code assumes floating-point inputs and calculations are finite; do not add `isfinite` / `isFinite` guards. Retain the relevant type, range, and index checks.
- When adding or changing Effect or native sampler configurations, explicitly set `addressU`, `addressV`, and `addressW` together, including for 2D textures. Keep all three modes consistent unless differing modes are required by the sampling behavior. An omitted `addressW` defaults to `WRAP`; setting only U/V to `CLAMP` triggers "Samplers with different wrapping modes may case reduced performance". This has recurred in Landscape's `vtPageMap`; check compiled sampler configurations after Effect changes and fix the configuration rather than suppressing the warning.
- During development, do not preserve compatibility with old data. Update affected code and data together, removing obsolete fields, migration paths, and compatibility fallbacks within the scope of the change. Only introduce backward compatibility when the user explicitly requests it.

# C++ engineering and architecture

- This is an open-source engine used by thousands of independent game projects. Do not hardcode project-specific Landscape dimensions, height ranges, resource layouts or tuning assumptions in engine code or shaders. Derive values from their owning assets/runtime configuration, and expose configuration when projects need to choose the behavior. Algorithm constants may remain internal when an additional public setting is not needed; keep them named and owned by the responsible module.

- Before implementation, identify module responsibilities, data owners, dependency direction and behavior that must remain unchanged. Check callers, build registration, generated bindings, resources and comments before moving public declarations.
- Keep each module focused on one coherent responsibility. Dependencies flow from coordination to functional modules to foundational data; foundational data must not depend on renderers, scenes or schedulers. Avoid circular dependencies.
- Place types by meaning and ownership: shared plain data in the appropriate foundational header, internal state inside its owning module, and GPU layouts/resources in the rendering layer. Do not turn a common data header into a catch-all.
- Keep one authoritative source for mutable state. Avoid storing values that can be obtained from the owner; when a derived cache is necessary, document its invalidation and update owner. Immutable configuration snapshots and GPU upload representations are not independent sources of truth.
- Make ownership and lifetime explicit: owning pointers, borrowed references and shared ownership must be distinguishable. Required dependencies must be initialized before runtime objects are published or used; do not hide failed initialization behind runtime null checks.
- Keep interfaces small and headers self-contained. Include complete definitions only when required; otherwise use forward declarations and put implementation dependencies in source files. Do not include a whole subsystem merely to obtain one data type.
- For asynchronous work, define preparation, readiness and commit boundaries. Preserve the last valid state until a complete replacement is ready; failures must not publish partially updated state.
- Prefer simple, local implementations. Add abstractions, files, special cases or stored state only when a concrete need and maintenance benefit justify them. Keep small related helpers together.
- Refactoring preserves behavior, data layout, defaults and externally visible interfaces unless a behavior change is explicitly approved. Review initialization failure and destruction order as well as the successful path.
- Match validation to the change: check dependency/build/binding integrity for structural changes, run authorized functional regression for behavior changes, and draw performance conclusions only from comparable Release measurements. Compilation alone does not prove behavioral equivalence.

# Keyboard shortcuts and window title

- Whenever PC controls are added or changed, update the mobile touch controls in the same change. Keep actions, camera modes, labels, debug options and input-reset behavior consistent across both, and verify keyboard/touch parity.
- Whenever a shortcut is added, removed, renamed, or changes behavior, update the native window title in D:\work\editors\projects\landscape\native\engine\common\Classes\Game.cpp in the same change.
- Keep the title consistent with the project shortcut definitions in assets/scripts/LandscapeDebug.ts and assets/scripts/FreeCamera.ts.
- Do not display mouse-operation hints in the title bar.

# Required reminders after changes

- After changing engine TypeScript code, explicitly remind the user to restart the editor.
- After changing Effects or project TypeScript code, explicitly remind the user to rebuild the project.
- After changing C++ code, explicitly remind the user to recompile the native code.
- After changing editor extension or importer code, explicitly remind the user to restart the editor. The editor may retain the old importer in memory until restarted.
- Do not remind the user to manually reimport Landscape resources. The editor automatically reimports them when resources are updated.
- When multiple change types are involved, list all required steps in execution order: restart the editor, rebuild the project, then recompile native code.
- Include the applicable reminders in the final response, stating which steps have already been completed and which remain for the user.

# Deferred Landscape work

- Multi-camera Landscape support is deferred until the user explicitly resumes it (recorded 2026-10-21). Do not implement it as part of unrelated refactoring.
- Current limitations: LOD selection uses one main camera; the committed Quadtree, morph camera parameters, decal visibility/fade and resource reservations are shared rather than independent per view.
- Proposed direction to evaluate when resumed: keep the committed Quadtree, draw instances and morph/decal view state per view; share height data, TilePagePool and VT atlas; aggregate resource demands across views before scheduling to avoid mutual eviction. Shadow passes should use their owning view's LOD and morph. Preserve existing single-camera behavior and morph transitions. Confirm whether the use case is camera switching, shared-LOD secondary views or simultaneous independent views before choosing the scope.

# Progress communication

- Before starting tool-assisted work, briefly tell the user what you will investigate or change.
- During ongoing work, provide a concise progress update at least once every 60 seconds. State what is confirmed, what remains uncertain, and what you will do next; do not leave the user waiting silently.
- If a tool, download, or external source stalls, promptly explain the delay and switch to a bounded retry or an alternative approach. Use short, resumable waits so tools do not prevent timely updates.
- Share useful partial findings as soon as they are available, clearly distinguishing verified facts from hypotheses. Do not delay the main explanation solely to chase secondary details or additional sources.
- When the user asks for status, answer immediately before continuing the task. When asked to pause, stop promptly and summarize the current state.
