# CircuitDrift

CircuitDrift is a planned audio effect plugin exploring dynamic circuits, component aging, temperature, power-supply sag and creative analog-inspired modulation.

**Status: framework-independent core under development. Parameter, thermal/protection, oversampling, synthetic MNA and state-document primitives have tests. Plugin wrappers, GUI and release binaries are not implemented. Physical hardware validation remains blocked.**

## Product and UX

- [Original product vision](docs/PRODUCT_VISION.md)
- [UX design brief](docs/UX_DESIGN_BRIEF.md): PCB Workbench, Easy Mode/Tone State Sphere, direct gestures, lifecycle and analytical displays
- [Planned feature register](docs/planned-features/FEATURE_REGISTER.md): all 23 requested features

## Engineering package

- [Specification index](docs/planned-features/README.md)
- [UX interaction and display contracts](docs/planned-features/08-ux-interaction-and-displays.md)
- [Decisions and resolved overlaps](docs/planned-features/DECISIONS.md)
- [Parameter registry](docs/planned-features/parameters.json)
- [Acceptance criteria](docs/planned-features/ACCEPTANCE.md)
- [Research sources and limits](docs/planned-features/SOURCES.md)
- [Open research/implementation gates](docs/planned-features/OPEN_QUESTIONS.md)
- [Implementation plan](tasks/FEATURE_IMPLEMENTATION_PLAN.md)
- [Specification review](docs/planned-features/SPEC_REVIEW.md)

## Continue development

Use the [consolidated development prompt](docs/prompts/CONTINUE_DEVELOPMENT.md), based on the [post-merge branch and handoff review](docs/BRANCH_REVIEW_2026-10-30.md). It prioritizes confirmed validation repairs, then synthetic process integration before dependent state queues.

Read [the handoff](docs/HANDOFF.md), [checkpoint](docs/CHECKPOINT.md), and [traceability](docs/TRACEABILITY.md) for current code and verification limits. The [integration review](docs/INTEGRATION_REVIEW.md) records branch reconciliation and safety findings.

The [original engine research/specification prompt](docs/prompts/ENGINE_SPECIFICATION_PROMPT.md) records the specification assignment and remaining physical-model research. Close those evidence gaps before claiming validated hardware emulation.

Requested builds are VST3, AU v2, AAX Native and CLAP on the applicable Windows/macOS architectures; adapter, licensing, signing and host validation remain planned work.

Validate the documentation package with:
```sh
python3 tools/validate_planned_features.py
```

That command checks specification consistency and example data. It does not build or test a plugin.
