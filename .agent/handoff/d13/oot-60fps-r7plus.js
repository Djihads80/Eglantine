export const meta = {
  name: 'oot-60fps-r7plus',
  description: 'Rounds 7+: measure -> research -> select -> implement+review (worktrees) -> integrate with flash/fog gates, toward a locked 60 FPS in Ocarina of Time',
  phases: [
    { title: 'Measure', detail: 'build HEAD, speed runs (or reuse last confirmation), flash baseline, profile + fence_timeline' },
    { title: 'Research', detail: 'parallel read-only lanes propose concrete changes' },
    { title: 'Select', detail: 'judge picks up to 4 non-conflicting candidates' },
    { title: 'Implement', detail: 'one agent per candidate in a worktree, switch default-off + verify; then an adversarial review' },
    { title: 'Integrate', detail: 'merge, verify, flash check, A/B, keep winners, confirm, fog check' },
  ],
}

const BRIEF = 'E:\\Projects\\Eglantine\\.agent\\prompts\\overnight\\BRIEF.md'
const RESULTS = 'E:\\Projects\\Eglantine\\.agent\\prompts\\overnight'
const FIRST_ROUND = 7
const MAX_ROUND = 16
const TRAILER = 'Assisted-by: Claude Opus 5.5'
const WT_CHECK = 'python -I E:/Projects/Eglantine/.agent/archive/2026-11-06-old-workflow/skills/nx2-worktrees/scripts/wt_check.py'
const WAIT_RULE = 'If eglantine.exe is running the user is playing: never kill it; wait with PowerShell `Start-Sleep 60` in a loop (check `Get-Process eglantine`), up to 2 hours, then continue. Kill only eden-cli.exe. The user is asleep: never stop to ask questions; decide and note the decision.'

const MEASURE_SCHEMA = { type: 'object', properties: {
  head_sha: { type: 'string' }, median_fps: { type: 'number' }, min_window_fps: { type: 'number' },
  device_lost: { type: 'boolean' }, flash_baseline: { type: 'string' }, gpu_fence_wait_ms: { type: 'number' },
  fog_score: { type: 'number' }, profile_summary: { type: 'string' },
  top_costs: { type: 'array', items: { type: 'object', properties: { thread: { type: 'string' }, item: { type: 'string' }, ms_per_frame: { type: 'number' } }, required: ['thread', 'item', 'ms_per_frame'] } },
}, required: ['head_sha', 'median_fps', 'min_window_fps', 'device_lost', 'profile_summary', 'top_costs'] }

const PROPOSALS_SCHEMA = { type: 'object', properties: { proposals: { type: 'array', items: { type: 'object', properties: {
  title: { type: 'string' }, files: { type: 'array', items: { type: 'string' } }, change: { type: 'string' },
  invariant: { type: 'string' }, expected_ms_per_frame: { type: 'number' }, risk: { type: 'string', enum: ['low', 'medium', 'high'] },
  timing_change: { type: 'boolean' }, verify: { type: 'string' }, probe_only: { type: 'boolean' },
}, required: ['title', 'files', 'change', 'invariant', 'expected_ms_per_frame', 'risk', 'timing_change', 'verify'] } } }, required: ['proposals'] }

const SELECT_SCHEMA = { type: 'object', properties: { selected: { type: 'array', items: { type: 'object', properties: {
  switch_name: { type: 'string' }, title: { type: 'string' }, brief: { type: 'string' }, files: { type: 'array', items: { type: 'string' } }, timing_change: { type: 'boolean' }, probe_only: { type: 'boolean' },
}, required: ['switch_name', 'title', 'brief', 'files', 'timing_change'] } }, reasoning: { type: 'string' } }, required: ['selected', 'reasoning'] }

const IMPL_SCHEMA = { type: 'object', properties: {
  ok: { type: 'boolean' }, branch: { type: 'string' }, sha: { type: 'string' }, worktree: { type: 'string' }, switch_name: { type: 'string' },
  files: { type: 'array', items: { type: 'string' } }, verify_checks: { type: 'string' }, not_verified: { type: 'string' }, timing_change: { type: 'boolean' },
}, required: ['ok', 'branch', 'sha', 'worktree', 'switch_name', 'verify_checks'] }

const REVIEW_SCHEMA = { type: 'object', properties: {
  ok: { type: 'boolean' }, sha: { type: 'string' }, issues_found: { type: 'string' }, fixed: { type: 'string' }, remaining_risk: { type: 'string' },
}, required: ['ok', 'sha', 'issues_found'] }

const INTEGRATE_SCHEMA = { type: 'object', properties: {
  head_sha: { type: 'string' },
  accepted: { type: 'array', items: { type: 'object', properties: { switch_name: { type: 'string' }, gain_fps: { type: 'number' }, evidence: { type: 'string' } }, required: ['switch_name', 'evidence'] } },
  rejected: { type: 'array', items: { type: 'object', properties: { switch_name: { type: 'string' }, reason: { type: 'string' } }, required: ['switch_name', 'reason'] } },
  confirm_median_fps: { type: 'number' }, confirm_min_window_fps: { type: 'number' }, device_lost: { type: 'boolean' },
  confirm_fog_score: { type: 'number' }, confirm_flash: { type: 'string' }, findings: { type: 'string' }, notes: { type: 'string' },
}, required: ['head_sha', 'accepted', 'rejected', 'confirm_median_fps', 'confirm_min_window_fps', 'device_lost', 'findings'] }

const LANES = [
  { key: 'front-waits', focus: 'THE main lever per round 6: the guest\'s fence wait is the FRONT not having parsed this frame\'s fence increment yet (fence_timeline: entered->front_parse 16-22 ms; back idles only ~4 ms of it). The front loses time waiting for the back at sync points: list writers (~100 ms/s profiled), acquire triggers (~140 ms/s), remaining macro DMA reads (dma_forward now covers macro 0x16; where did the waits move?). For each site, find exactly which value the front needs, who produces it (which back method/draw/copy/report), and design a way to get it without draining the back to the end of its ring: compute it on the front (as dma_forward recomputes render_enable), wait only for the exact producing ring position, let the back run producers first, or resolve GPU-written command lists differently. Already tried: acquire_trigger_early (+0.2), acquire_early (early reads see not-met: wrong direction), exact_noted_writes, writer_set_exec, front_sync_trio (-0.4), dma_forward_memo (3-4% hit). Read .agent/prompts/overnight/round6-base.md and round6-integrate.md for the per-site numbers.' },
  { key: 'front-cpu', focus: 'The front thread\'s own CPU cost on the critical path: macro JIT ~335 ms/s profiled (0.85M macro calls/s, macro 0x01 dominant: what does it do and can its effect be computed once per distinct input, or fused with the method writes it emits?), pushbuffer parsing ~200 ms/s (dma_pusher.cpp: per-header/per-word overhead, page lookups, FrontWrite per method), and the cost of pushing ring entries. Propose exact cuts with arithmetic. Lichii prior art (port the idea behind a switch, verify): f17682097a, c38631643a, 8d14da5e4b, 16b8285596 (MaxContinuousRange/GetMemoryLayoutSize from page runs kept at map time). Existing switches macro_batch, macro_borrow, macro_state, front_run, front_same_first, front_class_table are on: build on them, do not duplicate them.' },
  { key: 'safe-run-ahead', focus: 'Second lever (~+9 FPS measured when frames_ahead was on, but it flashed): let vi2 return before the back finished frame N without the guest\'s frame N+1 writes changing what the back reads for frame N. Round 6\'s inflight_write_wait (write-wait epochs on bank pages) failed: every epoch escaped at gameplay, ~20k bank pages changed before the fence, 711 reads differed. Before designing, measure from the code what the back reads lazily AFTER the front parsed a frame: uniform/constant buffer uploads (buffer cache SynchronizeBuffer, bind uniform bank), storage buffers, texture uploads, index/vertex data, query results; how many distinct bytes per frame; whether the front could capture them at parse time (e.g. constant buffer contents at bind) so the back never reads guest memory for frame N afterward. Propose either an exact design with its byte budget or a probe switch that measures it (counts bytes/pages the back reads from guest memory per frame after the present returns). Any switch here is timing_change=true and must pass the flash gate with 0 outliers.' },
  { key: 'back-throughput', focus: 'When the front waits for the back, the back\'s speed bounds the wait. Back thread: back replay (ring entry decode ~200 ms/s, ~65 ns/entry), method dispatch ~135 ms/s, draw setup ~200 ms/s (bind uniform bank, read handles, update buffers, pipeline lookup), Vulkan command recording/submission, fence handling. Find per-entry and per-draw overhead the back can drop (batching, cheaper dispatch, skipping entries whose effect is overwritten before the next draw) and changes that make the back reach the entries the front is waiting for sooner. Lichii prior art: 4a0bc9a007 (cheaper Maxwell3D dispatch on the back), 60e565c440 (keep deferred fragment barriers across draws). Existing: draw_begin_coalesce, bind_memo, draw_preamble on.' },
  { key: 'critical-path', focus: 'Build the per-frame critical path of one ~26 ms spawn-spot frame from existing evidence (r6-base-prof, r6-v-fence_timeline and r6-maxres-on-b logs in E:/Projects/Eglantine/.agent/lab/runs/, Nx2Frame lines, the profiler\'s waits-by-site table, vi2 present lines): guest CPU time, front busy vs waiting (by site), back busy vs idle, host GPU time per frame. Identify the single largest serial dependency chain (who waits on whom, how long) and either propose a concrete change that shortens it, or a probe switch (probe_only=true, off by default, cheap, logs per-second numbers) that would settle the open question, e.g. host GPU execution time per frame, how long each front wait site blocks per frame and which back work runs during it, or how much guest CPU time overlaps the front. Do not repeat probes that already exist (fence_timeline, war_guard_probe, GPU_PROFILE zones).' },
]

function measurePrompt(round, prevConfirm) {
  return `Read ${BRIEF} first (the "Current state for round 7+" section supersedes older numbers) and follow its Lab, Flashes and fog rules exactly. You are the ONLY agent allowed to build or run the emulator. ${WAIT_RULE} Round ${round} base measurement.
1. In E:\\Projects\\Eglantine (local-combined): tree must be clean (commit stray agent/doc files with trailer \`${TRAILER} (agent notes)\` if needed); note HEAD; build yuzu-cmd yuzu tests as in the brief; run tests.exe "[nx2_perf]".
2. ${prevConfirm ? `Reuse the previous round's confirmation runs as the speed and flash base IF no source file changed since they ran (check \`git log --stat\` since the round ${round - 1} integration commit: only .agent/ or docs changes allowed): ${prevConfirm}. Then skip to step 4. Otherwise do step 3.` : 'Do step 3.'}
3. Two speed runs, defaults: \`ab_run.ps1 -Name r${round}-base-a -GameplayWait 100 -MaxSeconds 420\` and -b (kill eden-cli and wait 5 s between). Pool with \`python -I .agent/lab/fps_windows.py r${round}-base-a r${round}-base-b\`: median, minimum. device_lost if either run shows DEVICE_LOST (emulator.log and eden_log.txt). Fog score of each capture-0.tga (fogscore.py; < 0.35 = fog regression: say so loudly in profile_summary). Flash baseline: \`ab_run.ps1 -Name r${round}-base-flash -Stay -GameplayWait 60 -Flash 40 -MaxSeconds 480\`; put its two "flash" lines in flash_baseline.
4. One profiled run \`-Name r${round}-base-prof -Env EDEN_NX2_GPU_PROFILE=1,EDEN_NX2_PERF_ON=fence_timeline -GameplayWait 100\`; summarize spawn-spot front/back zones, waits by site, counters, fence_timeline p50s, vi2 present lines as ms/frame (front zones nest waits; back flat; profiler inflates per-call zones). Top 10 costs in top_costs. gpu_fence_wait_ms = spawn-spot vi2 fence wait from a non-profiled run.
5. Write ${RESULTS}\\round${round}-base.md and commit it (.agent is git-ignored: \`git add -f\`; trailer \`${TRAILER} (measurement)\`). Return the structured result.`
}

function researchPrompt(round, lane, m, history) {
  return `Read ${BRIEF} first (the "Current state for round 7+" section and the Flashes rule). Read-only research lane "${lane.key}", round ${round}. Do not edit, build or run anything (reading lab run logs in E:/Projects/Eglantine/.agent/lab/runs/ is fine).
Focus: ${lane.focus}
Latest measurement (HEAD ${m.head_sha}, median ${m.median_fps} FPS, slowest ${m.min_window_fps}, GPU fence wait ${m.gpu_fence_wait_ms} ms; full notes in ${RESULTS}\\round${round}-base.md):
${m.profile_summary}
Top costs: ${JSON.stringify(m.top_costs)}
History (do not re-propose a tried idea unless you say what is different): ${history}
Read the code (cite file:line). Propose up to 4 concrete changes ranked by expected ms per frame saved ON THE CRITICAL PATH (a saving off the critical path, e.g. back work while the back idles, is worth ~0), with the arithmetic. Each must be gateable by a new Nx2Perf switch, identical when off, state its correctness invariant, say whether it changes timing (timing_change: when the guest/front/back may proceed or which value a wait/read returns), and how EDEN_NX2_PERF_VERIFY / SPLIT_VERIFY / the flash check would catch a mistake. Mark measurement-only proposals probe_only=true. Be skeptical of estimates: rounds 3-6 gained ~0 from most well-argued proposals. Return the structured proposals.`
}

function selectPrompt(round, m, proposals, history) {
  return `Read ${BRIEF} first. Judge for round ${round}: pick up to 4 proposals to implement in parallel, maximizing expected real frame-time saved toward 16.67 ms at acceptable risk. The front thread is the critical path (round 6): prefer proposals that shorten what the front does or waits for before it reaches the frame's fence increment, or that make run-ahead exact. Correctness beats speed: the user saw flashes from timing shortcuts; a proposal that cannot be verified exact is low priority. At most one probe_only proposal per round, and only if it settles a question that blocks a big lever.
Measurement: median ${m.median_fps}, slowest ${m.min_window_fps}. ${m.profile_summary}
History: ${history}
Proposals (JSON): ${JSON.stringify(proposals)}
Rules: mostly disjoint files; mix one or two big levers with safer ones; skip rejected ideas unless the reason is fixed. Give each a new snake_case switch_name (not already in src/video_core/nx2_perf.h opt_names) and a self-contained implementation brief (change, where with file:line, invariant, Verify design, identical-when-off), timing_change and probe_only. Return the structured selection.`
}

function implPrompt(round, sel, baseSha) {
  return `Read ${BRIEF} first (rules). Implementation, round ${round}: "${sel.title}", switch \`${sel.switch_name}\`${sel.probe_only ? ' (probe only: measurement, no behavior change even when on, besides logging)' : ''}. You work in your own git worktree (current directory). First \`git reset --hard ${baseSha}\` and confirm with \`git log -1\`. Never build with cmake/ninja and never run eden-cli or eglantine: check each edited .cpp (headers through a .cpp that includes them) with \`${WT_CHECK} <your-worktree-path> <file.cpp> ...\` until all pass.
Brief: ${sel.brief}
Files expected: ${(sel.files || []).join(', ')}
Requirements: add the switch to Opt, opt_names, DefaultOffMask() AND always_off in src/video_core/nx2_perf.h (off by default) and to the expected-off list in src/tests/video_core/nx2_perf.cpp's default-mask test. Off = byte-identical behavior. Keep video_core hot paths free of locks and heap allocation. Under Nx2Perf::Verify() compute the old answer too, LOG_ERROR(HW_GPU, "Nx2Perf ${sel.switch_name}: ...") on any difference (cap 64 lines, count all, log the count once per second or at shutdown), and use the old answer. Keep comments short. One commit, trailer \`${TRAILER} (implementation)\`; never Co-Authored-By; never push. Return the structured result with worktree = the absolute path of your worktree (ok=false with the reason if wt_check cannot pass).`
}

function reviewPrompt(round, sel, impl) {
  return `Read ${BRIEF} first (rules). Adversarial code review, round ${round}, switch \`${sel.switch_name}\`: commit ${impl.sha} on branch ${impl.branch} in worktree ${impl.worktree} (work only in that directory; never build with cmake, never run the emulator). The intended design: ${sel.brief}
Implementer's notes: verify_checks=${impl.verify_checks}; not_verified=${impl.not_verified || ''}.
Read the diff (\`git -C ${impl.worktree} show ${impl.sha}\`) and the surrounding code. Try to find REAL defects: (1) with the switch off, any behavior change (an ungated path, changed default, reordered side effect, extra lock/allocation on a hot path); (2) with it on, a data race between the guest, front and back threads, a stale read, a missed invalidation, or a case the invariant does not cover; (3) the Verify path not computing the old answer, not using it, or not logging differences; (4) the switch missing from Opt/opt_names/DefaultOffMask/always_off or the unit test's expected-off list; (5) compile issues. Fix real defects in that worktree with one additional commit (trailer \`${TRAILER} (review fix)\`), re-run \`${WT_CHECK} ${impl.worktree} <edited .cpp files>\` until it passes. Do not restyle or expand scope. ok=false only if the design is unsound and cannot be fixed in place (say why). Return sha = the branch tip after your fixes.`
}

function integratePrompt(round, impls, base, history) {
  return `Read ${BRIEF} first (the "Current state for round 7+" section) and follow its Lab, Flashes and fog rules exactly. You are the ONLY agent allowed to build or run the emulator. ${WAIT_RULE} Round ${round} integration in E:\\Projects\\Eglantine on local-combined (HEAD should be ${base.head_sha} or a later agent-notes commit).
Round baseline (defaults): median ${base.median_fps} FPS, slowest ${base.min_window_fps}, fog ${base.fog_score ?? '?'}. Flash baseline: ${base.flash_baseline || 'not measured: run one first (-Name r' + round + '-base-flash -Stay -GameplayWait 60 -Flash 40)'}.
Candidates (JSON, each reviewed): ${JSON.stringify(impls)}
History: ${history}
1. Merge each candidate branch (\`git merge --no-edit <branch>\`). nx2_perf.h lists conflict: keep every entry in order, renumber static_asserts, keep the unit test's expected-off list in sync; nx2_profile.h: keep all entries, size arrays to enum counts; check brace balance. Build yuzu-cmd yuzu tests; run tests.exe "[nx2_perf]" "[video_core]" "[split_writes]" "[native_macro]". Revert/reject a candidate you cannot fix in a few minutes. Commit merges (trailer \`${TRAILER} (integration)\`). Then remove each merged candidate's worktree and branch (\`git worktree remove <path>\`, \`git branch -d <branch>\`).
2. For each merged switch S: (a) Verify \`-Quick -Env EDEN_NX2_PERF_ON=S,EDEN_NX2_PERF_VERIFY=1,EDEN_NX2_SPLIT_VERIFY=1 -Name r${round}-v-S\`: any real mismatch => reject. For a probe_only switch, this run is the measurement: report its numbers in findings and leave it off. (b) If the candidate changes timing (timing_change) or touches waits, reads, fences, presents or submits: flash check \`-Name r${round}-f-S -Stay -GameplayWait 60 -Flash 40 -Env EDEN_NX2_PERF_ON=S\`; reject if outliers or stdev are clearly above the flash baseline (a run-ahead switch needs 0 outliers). (c) Speed: two runs \`-Env EDEN_NX2_PERF_ON=S -GameplayWait 100 -Name r${round}-s-S-a/-b\`; pool with fps_windows.py. Accept only if verify clean, flash clean, no DEVICE_LOST, game.png correct, fog score of capture-0.tga >= 0.35, and pooled median >= baseline + 1.0 FPS (or slowest window +2 with median not lower). If two accepted switches might interact, also run them together once.
3. Accepted switches: remove from DefaultOffMask() and always_off, update the unit test. Rebuild yuzu-cmd yuzu tests, run [nx2_perf]. Confirmation: two default speed runs \`r${round}-confirm-a/-b\` and one flash run \`r${round}-confirm-flash\`; report pooled median/min, flash lines (confirm_flash) and fog score (confirm_fog_score). If the confirmation is not better than the round baseline, say so plainly; if it is clearly worse (> 2 FPS), turn the round's accepted switches back off and say so.
4. Append a dated round summary to E:\\Projects\\Eglantine\\.agent\\STATUS.md (## Log, newest first; also refresh the "Performance" line of the HANDOFF section with the new numbers) and write ${RESULTS}\\round${round}-integrate.md. Commit the code and these notes (.agent is git-ignored: \`git add -f\` the STATUS and round files; trailer). In findings, write what the round taught about the critical path (for the next round's research). Return the structured result.`
}

let history = 'Round 1: submit_while_waited ACCEPTED (spawn 38.5->45.5); rejected macro_read_overlay_writers (+0.3), gpu_core_pin (-2.3). Round 2: present_one_ahead accepted (+9) but LATER REVERTED for incessant brightness flashes (guest rewrites data the back still reads); rejected present_guest_wait (guest panic), release_before_hook (+0.0), acquire_trigger_early (+0.2). Round 3: merged default-off pipeline_keep_preloaded, writer_set_exec, exact_noted_writes, gpu_core_pin separate/guest_ccd (no gain seen). frames_ahead (older default) found to be the real flash cause and turned off (~-5 ms/frame at the spawn). | Round 4: ACCEPTED bank_once_per_frame (~+1.7); rejected landed_prev_always (flash 8/30), one_ahead_landed (flash 7/30), split_rings (no gain), bind_memo_local (no gain). | Round 5 (base 36.6/34.1): ACCEPTED dma_forward (macro 0x16 occlusion read without a front drain; pooled 38.5) and draw_begin_coalesce (37.6); rejected guest_landed_syncpoints (+0.9), hook_lazy_flush (+1.9 slowest; verify: pending submits at fence waits); war_guard_probe measurement only. Confirmation 37.2/32.8 (gains did not add up). | Round 6 (base 36.8/35.4, flash sd 0.0020 0 outliers): rejected inflight_write_wait/inflight_read_exact (verify: every epoch escapes at gameplay, ~20k bank pages changed before the fence, 711 reads differ), front_sync_trio (-0.4), dma_forward_memo (-0.2, 3-4% hit); fence_timeline probe (off): guest fence wait = front not yet having parsed the fence increment (entered->front_parse 16-22 ms p50; back increments 7 us after; back idles ~4 ms of the wait) => FRONT IS THE CRITICAL PATH. Max Resolution patch: no measurable cost. Intermittent boot hang ~3.6-3.9 s (compute channel creation) seen twice. | Day session after round 6: fog fixed (OoT visual workaround made opt-in), upstream nx2 merged, paired-pointer shader fix, async shaders on by default; lab fast boot (gameplay marker 64x36x96 at ~37 s). Lab spawn ~37-39 FPS.'

let measurement = null
let prevConfirm = ''
let dry = 0
const rounds = []

for (let round = FIRST_ROUND; round <= MAX_ROUND; round++) {
  phase('Measure')
  measurement = await agent(measurePrompt(round, prevConfirm), { label: `measure r${round}`, phase: 'Measure', schema: MEASURE_SCHEMA })
  if (!measurement) { log(`round ${round}: measurement failed; stopping`); break }
  log(`round ${round}: HEAD ${measurement.head_sha} median ${measurement.median_fps}, slowest ${measurement.min_window_fps}, fog ${measurement.fog_score ?? '?'}`)
  if (measurement.median_fps >= 59.5 && measurement.min_window_fps >= 57 && !measurement.device_lost) { log('goal met'); break }

  phase('Research')
  const lanes = await parallel(LANES.map(lane => () =>
    agent(researchPrompt(round, lane, measurement, history), { label: `research ${lane.key} r${round}`, phase: 'Research', schema: PROPOSALS_SCHEMA })))
  const proposals = lanes.map((r, i) => r ? (r.proposals || []).map(p => ({ lane: LANES[i].key, ...p })) : []).flat()
  log(`round ${round}: ${proposals.length} proposals`)
  if (!proposals.length) { dry++; prevConfirm = ''; if (dry >= 3) break; continue }

  phase('Select')
  const sel = await agent(selectPrompt(round, measurement, proposals, history), { label: `select r${round}`, phase: 'Select', schema: SELECT_SCHEMA })
  const selected = (sel && sel.selected) ? sel.selected.slice(0, 4) : []
  log(`round ${round}: selected ${selected.map(s => s.switch_name).join(', ')}`)
  if (!selected.length) { dry++; prevConfirm = ''; if (dry >= 3) break; continue }

  phase('Implement')
  const impls = (await pipeline(selected,
    s => agent(implPrompt(round, s, measurement.head_sha), { label: `impl ${s.switch_name}`, phase: 'Implement', schema: IMPL_SCHEMA, isolation: 'worktree' }),
    (impl, s) => {
      if (!impl || !impl.ok) return null
      return agent(reviewPrompt(round, s, impl), { label: `review ${s.switch_name}`, phase: 'Implement', schema: REVIEW_SCHEMA })
        .then(rv => (rv && rv.ok) ? { ...impl, sha: rv.sha || impl.sha, review: rv.issues_found, review_fixed: rv.fixed || '', timing_change: s.timing_change, probe_only: !!s.probe_only } : null)
    })).filter(Boolean)
  log(`round ${round}: ${impls.length}/${selected.length} implementations ready after review`)
  if (!impls.length) { dry++; prevConfirm = ''; history += ` | round ${round}: all implementations failed (${selected.map(s => s.switch_name).join(', ')})`; if (dry >= 3) break; continue }

  phase('Integrate')
  const integ = await agent(integratePrompt(round, impls, measurement, history), { label: `integrate r${round}`, phase: 'Integrate', schema: INTEGRATE_SCHEMA })
  if (!integ) { log(`round ${round}: integration failed; stopping`); break }
  const acc = (integ.accepted || []).map(a => `${a.switch_name}(+${a.gain_fps ?? '?'})`).join(', ') || 'none'
  const rej = (integ.rejected || []).map(r => `${r.switch_name}: ${r.reason}`).join('; ') || 'none'
  history += ` | Round ${round} (base ${measurement.median_fps}/${measurement.min_window_fps}): accepted ${acc}; rejected ${rej}; confirm ${integ.confirm_median_fps}/${integ.confirm_min_window_fps}, fog ${integ.confirm_fog_score ?? '?'}. Findings: ${integ.findings}`
  log(`round ${round}: accepted ${acc}; confirm ${integ.confirm_median_fps} / ${integ.confirm_min_window_fps}`)
  rounds.push({ round, selected: selected.map(s => s.switch_name), integ })
  prevConfirm = `r${round}-confirm-a, r${round}-confirm-b (speed), r${round}-confirm-flash (flash), integration HEAD ${integ.head_sha}`
  dry = (integ.accepted || []).length ? 0 : dry + 1
  if (integ.confirm_median_fps >= 59.5 && integ.confirm_min_window_fps >= 57 && !integ.device_lost) { log('goal met after integration'); break }
  if (dry >= 3) { log('three rounds without an accepted change; stopping'); break }
}
return { rounds, history }
