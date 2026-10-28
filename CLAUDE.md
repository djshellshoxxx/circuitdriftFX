# CircuitDrift – working rules (stable; progress lives in docs/CHECKPOINT.md)

- Specs: docs/planned-features/README.md (read order), DECISIONS.md, ACCEPTANCE.md, OPEN_QUESTIONS.md (gates G01–G12). Plan: tasks/FEATURE_IMPLEMENTATION_PLAN.md (P01–P18). Traceability: docs/TRACEABILITY.md.
- Targets/dependency direction: `cd_core` (IDs, units, registry) <- `cd_dsp` (math core) <- [future] control, state, blueprint <- adapters/UI. Engine code never depends on UI, services, JUCE or wrappers.
- Audio thread: no alloc/locks/I/O/logging/graphics; bounded work; all process APIs noexcept; double precision state.
- `src/core/ParameterRegistry.generated.inc` is generated: edit parameters.json, run `python3 tools/gen_param_registry.py`. Host IDs 1000–1999 are permanent.
- Never claim physical validation: synthetic fixtures are labelled; G01 gates stay open. Never mark a requirement done from a spec, stub or mock.
- Build/test: `cmake -S . -B build -DCD_SANITIZE=ON && cmake --build build -j4 && ctest --test-dir build --output-on-failure`
- `tools/validate_planned_features.py` is documentation validation only.
