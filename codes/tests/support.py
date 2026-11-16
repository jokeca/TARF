"""Shared input and accounting helpers for simulator regression tests."""
import csv,hashlib,json,math
from pathlib import Path
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[2]

def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def dump(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n")

def load(path):
    def reject_constant(value):
        raise ValueError("nonfinite JSON value: " + value)
    return json.loads(Path(path).read_text(), parse_constant=reject_constant)

def artifact(path):
    path = Path(path).resolve()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": sha256(path)}

def geometry(name):
    # Dimensions: channels, chips/channel, dies/chip, planes/die, blocks/plane,
    # pages/block, bytes/page. OP excludes user-addressable sectors, not raw NAND.
    dims = {"debug32m": (1, 1, 1, 1, 64, 64, 8192),
            "16g": (4, 2, 2, 2, 256, 256, 8192),
            "64g": (16, 4, 2, 2, 256, 128, 8192),
            "micron16g": (2, 1, 1, 4, 64, 2112, 16384),
            "micron512gb": (8, 2, 2, 4, 128, 2112, 16384)}[name]
    size = 1
    for item in dims:
        size *= item
    expected = {"debug32m": 32 * 1024**2, "16g": 16 * 1024**3,
                "64g": 64 * 1024**3, "micron16g": 33 * 1024**3 // 2,
                "micron512gb": 528 * 1024**3}[name]
    if size != expected:
        raise ValueError("geometry capacity invariant violated")
    planes = dims[0] * dims[1] * dims[2] * dims[3]
    sectors_per_plane = dims[4] * dims[5] * (dims[6] // 512)
    op = 1 - 512_000_000_000 / size if name == "micron512gb" else 0.07
    result = {"name": name, "dimensions": dims, "raw_bytes": size,
              "overprovisioning_ratio": op,
              "user_sectors_conservative": planes * int(sectors_per_plane * (1 - op))}
    if name.startswith("micron"):
        result["hardware_provenance"] = {
            "technology": "TLC; supplied workbook 02 B5",
            "page_bytes": "16384 payload +1968 metadata; 02 B7",
            "pages_per_block": "2112; 02 B13",
            "planes_per_die": "4; 02 B11",
            "capacity_scaling": "16.5 GiB raw, 2 channels x1 chip x1 die; 64 blocks/plane; reduced die/block counts, not a full physical B47R package or paper 512GB SSD",
            "program_ns": 1459570,
            "program_source": "R TLC PE300,40C average 1459.57 us; 03 I6. Fixed for all page types, temperatures and P/E; per-page-type curves unavailable",
            "read_ns": 73000,
            "read_source": "upper end of R 56-73 us summary, 10 B6; fixed conservative scenario, not measured mean",
            "erase_ns": 10000000,
            "erase_source": "inherited baseline assumption; not measured in workbook",
            "block_pe_limit": "10000 simulation bookkeeping limit, not endurance specification; R rated TLC 3000",
        }
        if name == "micron512gb":
            result["hardware_provenance"]["capacity_scaling"] = "528 GiB raw; nominal 512 decimal GB user capacity (per-plane sector rounding applies); 8 channels, 2 chips/channel, 2 dies/chip, 4 planes/die, 128 blocks/plane. SSD topology is a simulation design, not measured package topology."
    return result

def write_config(path, geom, seed):
    # The legacy template declares ASCII despite containing Chinese comments.
    tree = ET.ElementTree(ET.fromstring((ROOT / "codes/ssdconfig.xml").read_text()))
    changes = {"Seed": seed, "Enabled_Preconditioning": "true",
               "IO_Queue_Depth": 4096, "Queue_Fetch_Size": 64,
               "Ideal_Mapping_Table": "true", "Data_Cache_Capacity": 1048576,
               "Use_Copyback_for_GC": "false", "Preemptible_GC_Enabled": "false",
               "Dynamic_Wearleveling_Enabled": "false", "Static_Wearleveling_Enabled": "false",
               "Set_count": 1, "Block_PE_Cycles_Limit": 10000,
               "Overprovisioning_Ratio": geom["overprovisioning_ratio"]}
    tags = ("Flash_Channel_Count", "Chip_No_Per_Channel", "Die_No_Per_Chip",
            "Plane_No_Per_Die", "Block_No_Per_Plane", "Page_No_Per_Block", "Page_Capacity")
    changes.update(zip(tags, geom["dimensions"]))
    if geom["name"].startswith("micron"):
        changes.update({"Flash_Technology": "TLC", "Page_Metadat_Capacity": 1968,
                        "Block_Erase_Latency": geom["hardware_provenance"]["erase_ns"]})
        for page_type in ("LSB", "CSB", "MSB"):
            changes["Page_Read_Latency_" + page_type] = geom["hardware_provenance"]["read_ns"]
            changes["Page_Program_Latency_" + page_type] = geom["hardware_provenance"]["program_ns"]
    for tag, value in changes.items():
        element = tree.find(".//" + tag)
        if element is None:
            raise ValueError("missing SSD template field: " + tag)
        element.text = str(value)
    ET.indent(tree)
    tree.write(path, encoding="utf-8", xml_declaration=True)

def write_workload(path, trace, geom):
    root = ET.Element("MQSim_IO_Scenarios")
    scenario = ET.SubElement(root, "IO_Scenario")
    flow = ET.SubElement(scenario, "IO_Flow_Parameter_Set_Trace_Based")
    values = {"Priority_Class": "HIGH", "Device_Level_Data_Caching_Mode": "TURNED_OFF",
              "Initial_Occupancy_Percentage": 80, "File_Path": str(trace.resolve()),
              "Percentage_To_Be_Executed": 100, "Relay_Count": 1,
              "Total_requests_to_server": 0, "Choose_type": 0, "Turn_on": 0,
              "Time_Unit": "NANOSECOND"}
    for tag, count in zip(("Channel_IDs", "Chip_IDs", "Die_IDs", "Plane_IDs"), geom["dimensions"]):
        values[tag] = ",".join(map(str, range(count)))
    for tag, value in values.items():
        ET.SubElement(flow, tag).text = str(value)
    tree = ET.ElementTree(root)
    ET.indent(tree)
    tree.write(path, encoding="utf-8", xml_declaration=True)

def temperature_points(path):
    points = []
    with Path(path).open() as stream:
        for row in csv.reader(stream):
            if row and not row[0].startswith("#") and row[0] != "time_ns":
                points.append((int(row[0]), float(row[1])))
    return points

def audit_migrations(path, rel, manifest):
    points = temperature_points(manifest["common_environment"]["TARF_TEMPERATURE_FILE"])
    initial_pe = int(manifest["common_environment"]["TARF_INITIAL_PE"])
    initial_age = float(manifest["common_environment"]["TARF_INITIAL_AGE_DAYS"])
    initial_states = None
    state_file = manifest["common_environment"].get("TARF_INITIAL_STATE_FILE")
    if state_file:
        with Path(state_file).open() as stream:
            initial_states = {int(row["block_index"]): row for row in csv.DictReader(stream)}
        initial_age = max(float(row["age_days"]) for row in initial_states.values())
    failures, count = [], 0
    if not path.exists():
        return {"passed": False, "rows": 0, "failures": ["migration audit file missing"]}
    with path.open() as stream:
        for row in csv.DictReader(stream):
            count += 1
            completion = int(row["completion_ns"])
            destination_initial_pe = initial_pe
            if initial_states is not None:
                _, chips, dies, planes, blocks, _, _ = manifest["geometry"]["dimensions"]
                index = (((int(row["source_channel"]) * chips + int(row["source_chip"])) * dies
                          + int(row["source_die"])) * planes + int(row["source_plane"]))
                index = index * blocks + int(row["destination_block"])
                destination_initial_pe = int(initial_states[index]["pe"])
            expected_temp = points[0][1]
            for stamp, temp in points:
                if stamp > completion:
                    break
                expected_temp = temp
            checks = {
                "actual destination retains physical P/E": destination_initial_pe <= int(row["destination_pe"]) <= rel["pe_max"],
                "program time set at physical completion": int(row["program_ns"]) == completion,
                "program temperature from completion trajectory": math.isclose(float(row["program_temp_c"]), expected_temp, abs_tol=1e-10),
                "initial retention age reset on new program": float(row["initial_age_days"]) == 0,
                "source invalidated after completed migration": row["source_live_after"] == "0",
                "destination contains live data": row["destination_live"] == "1",
                "migration uses distinct destination block": row["source_block"] != row["destination_block"],
                "no age acceleration": 0 <= float(row["source_age_before_days"]) <= initial_age + completion / 86400000000000 + 1e-9,
                "physical completion within horizon": 0 <= completion <= manifest["horizon_ns"],
            }
            failures.extend(f"row {count}: {name}" for name, passed in checks.items() if not passed)
    expected_rows = min(2048, rel.get("refresh_write_pages_completed", 0))
    if count != expected_rows:
        failures.append(f"audit rows {count} != first {expected_rows} completed refresh programs")
    return {"passed": not failures, "rows": count, "sample_limit": 2048, "failures": failures[:50]}

def audit_risk_episodes(path, rel, manifest):
    failures, count, censored, completed, total_wait, maximum = [], 0, 0, 0, 0, 0
    if not path.exists():
        return {"passed": False, "rows": 0, "failures": ["risk episode audit file missing"]}
    with path.open() as stream:
        for row in csv.DictReader(stream):
            count += 1
            start, end, wait, censor = [int(row[k]) for k in
                                       ("first_observed_above_ns", "end_ns", "observed_wait_ns", "censored_at_horizon")]
            if not 0 <= start <= end <= manifest["horizon_ns"] or wait != end - start:
                failures.append(f"row {count}: inconsistent risk episode duration")
            if censor not in (0, 1) or (censor and end != manifest["horizon_ns"]):
                failures.append(f"row {count}: invalid final censoring")
            if int(row["scan_resolution_ns"]) != manifest["scan_interval_ns"]:
                failures.append(f"row {count}: sampling resolution differs from manifest")
            censored += censor
            completed += 1 - censor
            total_wait += wait
            maximum = max(maximum, wait)
    if censored != rel.get("pending_risky_blocks"):
        failures.append("censored episode count does not equal final risky blocks")
    if completed != rel.get("completed_risk_episodes"):
        failures.append("completed episode count differs from aggregate")
    if not math.isclose(total_wait / 1e9, rel.get("risky_block_seconds", -1), rel_tol=1e-9, abs_tol=1e-8):
        failures.append("risk exposure omits or double-counts an episode")
    if not math.isclose(maximum / 1e9, rel.get("max_over_threshold_wait_seconds", -1), abs_tol=1e-8):
        failures.append("max wait differs from per-block episodes including censored ones")
    return {"passed": not failures, "rows": count, "censored": censored,
            "completed": completed, "failures": failures[:50]}
