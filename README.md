# Factory Sim

A discrete-event pipeline simulator written in C++20, with a browser-based visual editor (**Factory Studio**) and a one-click **audit and health report (PDF)** for any pipeline you design.

Build a production line, service desk or software pipeline out of sources, machines, conveyors, batchers, assemblers, quality gates and sinks. Set how every node behaves and how nodes hand work to each other. Then run it: watch parts flow, buffers fill and machines starve, block and break down. The simulator finds the bottleneck, measures what would fix it, and can write you a PDF report grading the design.

* **C++20 engine**: min-heap event queue, integer-microsecond clock, bounded buffers with backpressure, MTBF/MTTR failures, deadlock detection, bit-reproducible runs.
* **Pipeline sandbox**: any directed graph; routing rules, rework loops, parallel lines, batching, kitting, changeovers, priorities. 16 ready-made examples from hospitals to semiconductor fabs.
* **Custom nodes**: add a node type with a ~40-line C++ class, or define reusable types in JSON. They show up in config files, the validator and the Studio UI automatically.
* **Factory Studio**: drag-and-drop editor, animated replay, charts, what-if analysis, confidence intervals, standalone exportable replays.
* **Audit and health report**: 0-100 health score, graded findings with recommended actions, reliability and robustness analysis, as a multi-page PDF.

---

## 1. What you need

Everything below is free. You need **a compiler and CMake**; the project downloads its own libraries.

| Requirement | Why | Download |
|---|---|---|
| **A C++20 compiler** | builds the simulator | see the table for your system below |
| **CMake 3.20 or newer** | build system | <https://cmake.org/download/> (or the package manager command below) |
| **Internet access, once** | the first configure step downloads three small header-only libraries (nlohmann/json, cpp-httplib, doctest), about 10 MB in total | nothing to install, CMake does it |
| A web browser (Chrome, Edge, Firefox, Safari) | to use Factory Studio | any current browser |
| Git (optional) | to clone the repository; you can also use GitHub's "Download ZIP" | <https://git-scm.com/downloads> |

You do **not** need Python, Node.js, Docker, or any paid software.

**Compiler, by operating system**

| System | Install this | Notes |
|---|---|---|
| **Windows 10/11** (recommended) | **Visual Studio 2022 Build Tools** from <https://visualstudio.microsoft.com/downloads/> (scroll to "Tools for Visual Studio", pick *Build Tools for Visual Studio 2022*). In the installer tick the workload **Desktop development with C++**. | Also install CMake from cmake.org and tick "Add CMake to the system PATH" in its installer. |
| Windows (alternative) | **MSYS2** from <https://www.msys2.org/>, then in the *MSYS2 UCRT64* shell: `pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja` | GCC 12 or newer. |
| **Ubuntu / Debian** | `sudo apt update && sudo apt install -y build-essential cmake git` | Needs GCC 12+ (Ubuntu 22.04 or newer). On older systems install `g++-12` and add `-DCMAKE_CXX_COMPILER=g++-12`. |
| **Fedora** | `sudo dnf install -y gcc-c++ cmake git` | |
| **macOS** | `xcode-select --install`, then `brew install cmake` (Homebrew: <https://brew.sh>) | Apple Clang 14 or newer. |

Check that CMake is installed:

```bash
cmake --version        # must print 3.20 or higher
```
On Windows with Visual Studio, open **"Developer PowerShell for VS 2022"** from the Start menu (it has the compiler on its PATH) and run all commands there. On Linux and macOS use a normal terminal.

---

## 2. Get the code, build, and run

### Step 1: get the code

```bash
git clone https://github.com/Zephyr4772/factory-sim.git
cd factory-sim
```
(or download the ZIP from the GitHub page, unzip it, and open a terminal in the unzipped folder)

### Step 2: build

Linux, macOS, or Windows with MSYS2 / Ninja:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Windows with Visual Studio (in *Developer PowerShell for VS 2022*):

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
```

The first configure downloads the libraries and takes a minute; the build takes one to three minutes. When it finishes, the program is here:

| System | Program |
|---|---|
| Linux, macOS, MSYS2 | `build/factory_sim` |
| Windows + Visual Studio | `build\Release\factory_sim.exe` |

The `web/` and `examples/` folders are copied next to the program automatically.

### Step 3: check that it works (optional but recommended)

```bash
ctest --test-dir build -C Release --output-on-failure
```
You should see `100% tests passed`.

### Step 4: start Factory Studio

```bash
./build/factory_sim serve --open            # Linux / macOS / MSYS2
build\Release\factory_sim.exe serve --open  # Windows + Visual Studio
```

Your browser opens at **http://127.0.0.1:8080** with an example already running. Keep the terminal window open while you use it; press `Ctrl+C` there to stop. The server only listens on your own machine (127.0.0.1). Use `--port 9000` if 8080 is taken.

> The commands below are written for Linux/macOS (`./build/factory_sim`). On Windows + Visual Studio, replace it with `build\Release\factory_sim.exe`.

---

## 3. Using it

### Factory Studio

| To do this | Do this |
|---|---|
| Try an example | Pick one from **Load example** in the top bar (16 included) |
| Add a node | Click a type in the left panel. If a node is selected the new one is wired after it. |
| Connect nodes | Select a node, then drag the round handle on its right edge onto another node |
| Edit a node | Select it. The right panel shows every parameter with its explanation. |
| Move / zoom | Drag a node to move it, scroll to zoom, drag the background to pan |
| Delete | Select and press `Del` |
| Run | **Run simulation** (or `Ctrl+Enter`). Use the play bar to scrub through time. |
| Find the best improvement | **What-if** tab: measures the throughput gain of faster machines, bigger buffers and shorter repairs |
| Check result stability | **Replications** tab: 20 seeds, mean with 95% confidence interval |
| Get a PDF report | **Audit report (PDF)**, see section 4 |
| Share a replay | **Export replay** downloads one self-contained HTML file anyone can open |
| Save / open | **Save file** / **Open file** use the same JSON the command line reads |
| Reusable node | Select a node, **Save as node type**; it appears under *My node types* |

### Command line

```bash
./build/factory_sim run examples/bike_assembly.json                  # simulate and print the report
./build/factory_sim run examples/bike_assembly.json --whatif --reps 20
./build/factory_sim run examples/bike_assembly.json --html replay.html --csv wip.csv --json results.json
./build/factory_sim audit examples/bike_assembly.json --pdf audit.pdf   # audit and health report
./build/factory_sim validate my_pipeline.json                         # check a file without simulating
./build/factory_sim types                                             # list node types and parameters
```
Exit codes: `0` success, `2` configuration error, `3` the run ended in a deadlock, `5` audit score below `--fail-below`.

---

## 4. The audit and health report

Click **Audit report (PDF)** in Studio, or run:

```bash
./build/factory_sim audit my_pipeline.json --pdf audit.pdf
```

It simulates the pipeline, repeats the run with 10 different random seeds, tries improvement levers, inspects the structure, and writes a multi-page PDF. No extra software is needed to generate it. Samples for every example are in [`showcase/audits/`](showcase/audits/).

| Section | What it tells you |
|---|---|
| **Health score** | 0-100 and a grade A-F, with the number of critical, high, medium, low and info findings |
| **Executive summary** | The diagnosis, what to fix first, and the best *measured* improvement |
| **Findings** | Each issue: severity, where it is, why it matters, the recommended action, and the points it cost |
| **Performance by node** | Busy / blocked / down / starved share per node as bars, queue sizes, and a work-in-process chart |
| **Reliability and robustness** | Throughput with 95% confidence over replications, deadlocks across random seeds, failure model, single points of failure |
| **Model validity** | Unit conservation, Little's Law, run length, warm-up, deadlock-free: PASS / WARN / FAIL |
| **What-if analysis** | Measured throughput change for each lever |
| **Appendices** | Effective parameters of every node, and exactly how the score was computed |

**What it checks:** deadlock-prone loops (found at run time and structurally), lost arrivals, a saturated bottleneck, nodes that are blocked or down a lot, unbounded buffers, stations every unit depends on that can fail (an assembler needs *all* of its inputs), missing failure data, results that vary too much between seeds, and whether the run is long enough to trust. The score is 100 minus the listed penalties, with caps so one repeated issue cannot dominate. For automation, `--fail-below 70` makes the command exit with code 5 when the score is lower.

---

## 5. Examples

Open any of them from **Load example** in Studio, or run `factory_sim run examples/<name>.json`.

| File | Domain | What it demonstrates |
|---|---|---|
| `sample_factory.json` | Machining | Parallel assembly cells with shortest-queue routing, an inspection **rework loop** (bypass lane) and scrap. |
| `bike_assembly.json` | Manufacturing | **Custom nodes**: assembler (1 frame + 2 wheels), `quality_gate` with a rework budget, batcher, conveyor and a user-defined `cnc_line` node type. |
| `mixed_products.json` | Manufacturing | Part types, **routing rules**, priority queue, per-type process times, changeover times. |
| `bottling_line.json` | Beverage | A high-speed line with small buffers and a **case packer** (batches of 12); the filler is the constraint. |
| `pcb_assembly_line.json` | Electronics | SMT line with a reflow oven, AOI inspection (`quality_gate`), rework bench and scrap; a failing chip shooter limits output. |
| `car_final_assembly.json` | Automotive | Body and engine supply lines meeting in an **assembler**, final inspection with rework, paint booth as the constraint. |
| `semiconductor_fab.json` | Semiconductors | **Re-entrant flow**: wafer lots loop back to lithography for each layer through a bypass lane. |
| `coffee_roastery.json` | Food and drink | **Changeovers** between coffee origins, degassing delay, cupping QC, bagging in batches. |
| `pizza_kitchen.json` | Hospitality | Routing **rules by item type** to different stations, shared ovens, delivery runs batched in pairs. |
| `warehouse_fulfillment.json` | Logistics | Pickers and pack stations with **express priority**, a repick loop, a sorter by rule and timed truck loads. |
| `airport_security.json` | Transport | Fast-track vs standard lanes, shortest-queue X-ray lanes that break down, secondary search. |
| `emergency_department.json` | Healthcare | Priority **triage**, critical patients to a trauma bay by rule, exam rooms, lab, discharge, walk-outs when full. |
| `call_center.json` | Services | IVR delay, agent pool, **abandoned calls** (drop-on-full), escalation to specialists and callbacks. |
| `ci_cd_pipeline.json` | Software | Build runners that crash, flaky-test retries, hotfix priority, docs skipping integration, shared test environments. |
| `deadlock_demo.json` | Teaching | A rework loop with no slack: the simulator detects the deadlock and prints the cycle. |
| `legacy_spec.json` | Compatibility | The original `ingestion` + `stations` format, converted on load. |

Pre-generated outputs: `showcase/*.html` (open in a browser: interactive replays, no install needed) and `showcase/audits/*.pdf` (audit reports).

---

## 6. Troubleshooting

| Symptom | Fix |
|---|---|
| `cmake: command not found` / not recognised | Install CMake (section 1) and reopen the terminal. On Windows tick "Add CMake to the system PATH". |
| CMake says *No CMAKE_CXX_COMPILER could be found* (Windows) | Install the **Desktop development with C++** workload, then run from **Developer PowerShell for VS 2022**. |
| Compile errors mentioning `<span>`, `<concepts>` or other C++20 features (Linux) | Your compiler is too old. Use GCC 12+ / Clang 15+: `sudo apt install g++-12` and configure with `-DCMAKE_CXX_COMPILER=g++-12`. |
| Configure fails while downloading (timeout, 404, hash mismatch) | Check your internet / proxy / firewall and run the configure command again. Delete the `build` folder first if it was half-created. |
| Studio opens but shows a blank page or "cannot reach the server" | Make sure the terminal running `serve` is still open. Hard-refresh the page (`Ctrl+F5`). |
| `cannot listen on port 8080 (already in use?)` | Use another port: `factory_sim serve --port 9000`. |
| `could not find the web/ directory` | Run the program from the project folder, or set the environment variable `FACTORY_SIM_HOME` to the folder that contains `web/` and `examples/`. |
| Windows Firewall asks about the program | It only listens on 127.0.0.1; clicking Cancel or Allow both work. |
| A run ends in `DEADLOCK` | Not a bug in the tool: your design has a circular wait. The report names the cycle; add `"bypass": true` to a rework edge or an unbounded buffer. |

---

## 7. The config format

```jsonc
{
  "simulation": { "duration_seconds": 7200, "random_seed": 42, "warmup_seconds": 600,
                  "sample_interval": 30, "stop_on_deadlock": true },
  "node_types": {                                   // your own reusable types
    "cnc_line": { "base": "machine", "params": { "mtbf": 1200, "mttr": 120, "on_failure": "restart" } }
  },
  "nodes": [
    { "id": "mill", "type": "cnc_line", "label": "CNC Milling", "x": 200, "y": 300,
      "capacity": 5,
      "process": { "dist": "normal", "mean": 10, "stddev": 1.5 },
      "routing": { "policy": "shortest_queue",
                   "rules": [ { "match": { "type": "A" }, "to": "line_a" }, { "to": "line_b" } ] },
      "downstream": [ "line_a",
                      { "to": "line_b", "weight": 0.3, "rework": true, "bypass": true } ] }
  ]
}
```

Node parameters may sit directly on the node or inside `"params"`. Unknown parameters are **rejected with the node and parameter name**. `factory_sim types` prints every node type and parameter.

**Distributions** (seconds): a number (constant) or `{"dist": ...}` with `constant{value}`, `exponential{mean|rate}`, `normal{mean,stddev}` (resampled until positive), `uniform{min,max}`, `triangular{min,mode,max}`, `lognormal{mean,stddev}`.

**Edges** (`downstream`): `to`, `weight` (relative share), `rework` (counts a rework), `bypass` (target accepts even when full: a rework lane that breaks circular waits), `label`.

**Routing**: `policy` is `probabilistic`, `round_robin`, `shortest_queue` or `first_available`. `rules` are first-match filters over `type`, `quality_lt/ge`, `reworks_lt/ge`, `priority_ge`.

### Built-in node types

| Type | Behaviour | Key parameters |
|---|---|---|
| `source` | Creates parts with random inter-arrival times and a part-type mix. | `interarrival`, `mix`, `priority`, `on_blocked` (`drop` or `wait`), `max_parts` |
| `machine` | Bounded input buffer, stochastic service, failures, changeovers. | `capacity`, `process`, `process_by_type`, `setup`, `discipline`, `on_full`, `mtbf`, `mttr`, `failure_clock`, `on_failure` |
| `batcher` | Groups N parts (or whatever waited `timeout` seconds) into one batch. | machine params + `batch_size`, `timeout`, `output_type` |
| `assembler` | Waits for a complete kit and merges it. `capacity` is **per part type**. | machine params + `kit`, `output_type` |
| `delay` | A conveyor: unlimited parallel transit, order-preserving, can block at the end. | `transit`, `capacity` |
| `sink` | Terminal node. | `counts_as` (`completed` or `scrapped`) |
| `quality_gate` | **Custom example**: inspection with a quality threshold and rework budget. | machine params + `threshold`, `max_reworks`, `pass_to`, `rework_to`, `scrap_to` |

## 8. Writing your own node

Everything lives in [`src/custom_nodes.cpp`](src/custom_nodes.cpp); `quality_gate` is the worked example.

1. **Derive** from `Node` (blank slate) or `Machine` (buffer + service + failures) and override only the hooks you need:

   ```cpp
   class QualityGate final : public Machine {
   public:
       std::string_view kind() const override { return "quality_gate"; }
       void resolve() override { pass_ = find_edge(cfg_.pass_to); }   // names -> edge indices
   protected:
       std::optional<int> route_override(const Part& p) override {    // choose the destination edge
           return p.quality >= cfg_.threshold ? pass_ : rework_;
       }
   };
   ```
2. **Describe** its parameters and write a factory that validates them (`ParamReader` rejects typos), then add it with `reg.add({...})`.
3. **Register** it in `register_custom_nodes()` and rebuild. It now works in config files, `factory_sim types`, the Studio palette and property panel, and the audit report.

Hooks on `Machine`: `pick_work`, `make_output`, `on_service_done`, `after_enqueue`, `on_timer`. On any `Node`: `can_accept`/`accept`, `on_event`, `on_space`, `route_override`, `start`, `resolve`. For configuration-only variants no C++ is needed: use `node_types` or *Save as node type*.

## 9. How it works

```
config.json -> config_loader -> Plant { nodes, edges, parts pool } <-> EventEngine (min-heap on (time, seq))
                                   |                                     |
                                   +-- Node::on_event handlers <---------+
                                   +-- analytics -> audit -> PDF report
```

* **Events**: `PART_SPAWNED`, `PROCESSING_COMPLETED`, `BUFFER_SLOT_FREED`, `MACHINE_FAILED`, `MACHINE_REPAIRED`, `TRANSIT_COMPLETED`, `TIMER`, `SAMPLE`, `WARMUP_END`, `SIMULATION_END`.
* **Backpressure**: a node holds its finished part until a downstream node can accept it (state `BLOCKED`) and queues on that node's wait list; when it frees a slot, waiters retry in the order they blocked.
* **Stale events**: a breakdown mid-cycle bumps the machine's generation counter, so the pending completion is recognised as stale and dropped.
* **Determinism**: integer-microsecond ticks, ties broken by sequence number, one `mt19937_64` stream per (node, purpose) seeded from the node's name, hand-written samplers: identical results on every compiler and platform.
* **Deadlock detection**: when nodes are blocked, nothing that could unblock them is scheduled and no arrival could restart the line, the run stops and reports the wait-for cycle.
* **Conservation**: `created = completed + scrapped + lost + in-system` after every run, including through batching and assembly.
* **Little's Law**: `L` from the time-integral of WIP, `lambda` and `W` from departed units; the residual gap is the units still in flight at the cut-off.
* **Bottleneck index**: `(busy% + down%) + 0.1*avg(downstream starved%) + 0.1*avg(upstream blocked%)`; utilisation dominates and the neighbour terms break ties. What-if analysis is the arbiter because it measures the real change.
* **Replications**: seeds `seed .. seed+K-1` run on a thread pool; 95% intervals use Student's t.

## 10. Tests

`ctest --test-dir build -C Release` runs 60 test cases: event ordering, determinism, backpressure, deadlock detection, failures, every node type, config validation, conservation over many seeds, every shipped example, the audit and PDF writer, and **analytic validation** against queueing theory (M/M/1, M/M/1/K blocking probability, Little's Law, tandem-line throughput).

## 11. Project layout

```
include/factory/   engine, nodes, plant, registry, config_loader, analytics, audit, pdf, server headers
src/               implementations; custom_nodes.cpp (your nodes); main.cpp (CLI); server.cpp (Studio server);
                   audit.cpp + audit_pdf.cpp + pdf.cpp (report)
web/studio.html    the whole Studio front end (no build step, no dependencies)
examples/          16 pipelines            tests/    doctest suite
showcase/          pre-generated replays and audit PDFs            docs/   design notes + explainer
```
Design notes and the reasoning behind the architecture: [docs/SCOPE.md](docs/SCOPE.md). Animated explainer: [docs/visual.html](docs/visual.html).
