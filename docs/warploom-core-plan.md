# warploom-core: S2 dependency analysis and migration plan
  declares `omnicpp::core` then `omnicpp::render`; never `omnicpp::editor`.
  editor widgets and the render recorder keep their current home; they will
**warploom-core does NOT take**: `node_editor`/`inspector` (widget layer -
## Strategy: phase A (this session): the package, zero churn
## Strategy: phase B (later session): identifier migration
   `engine/core/...` via the superproject include root; forwarders keep
   is the canary for "extraction changed behavior"; it must stay green and
Status: **DONE** (verified per the gate below); the contract is the
   `warploom::contract`. File-scoped, mechanical openers + closers -
   warploom::core; }`) is ill-formed for namespaces; `using X = ...`
   Include roots do NOT change; only namespace spelling moves.
   repo rename, binary names) stays OUT; it rides with S5 so adopters
     `G3B_CONSUMER_OK`; the last two now exercise the alias path, since