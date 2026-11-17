"""
Empirical Challenger Adversarial Stress Suite (tests/stress_adversarial_suite.py)
Exercises concurrency limits, thread safety, KeyboardInterrupt handling, and race conditions.
"""

import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time
from typing import List

from PIL import Image
import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
CLIENT_DIR = REPO_ROOT / "clients" / "python"
sys.path.insert(0, str(CLIENT_DIR))

from cpower_dataset_collector import CPowerDatasetCollector
from cpower_continual_loop import (
    run_continual_cycle_safe,
    worker_explorer_agent,
    worker_web_agent,
    worker_dialog_agent,
)
from cpower_dataset_validator import validate_dataset


def run_experiment_1_high_concurrency_collector(temp_dir: Path) -> dict:
    """Stress tests CPowerDatasetCollector under 50 concurrent threads writing 10 steps each (500 steps)."""
    collector = CPowerDatasetCollector(base_dir=temp_dir, min_free_gb=0.001)
    threads_count = 50
    steps_per_thread = 10
    total_expected = threads_count * steps_per_thread
    errors: List[str] = []

    arabic_phrases = [
        "زر بدء التشغيل السريع",
        "مجلد ملفات النظام المتقدمة",
        "قائمة الإعدادات والتفضيلات",
        "حفظ التغييرات في المسار C:\\Data\\تصدير",
        "تأكيد العملية وإغلاق النافذة",
    ]

    def worker_task(thread_id: int):
        try:
            tid = collector.start_trajectory(f"StressTraj-{thread_id}")
            for s in range(steps_per_thread):
                img = Image.new("RGB", (120, 80), ((thread_id * 5) % 255, (s * 25) % 255, 128))
                phrase = arabic_phrases[(thread_id + s) % len(arabic_phrases)]
                collector.record_step(
                    trajectory_id=tid,
                    image=img,
                    action_type="click",
                    phrase=phrase,
                    bbox=(10, 10, 100, 70),
                    click_coords=(50, 40),
                    window_title=f"Stress Window {thread_id}",
                    metadata={"thread_id": thread_id, "step": s},
                )
            collector.complete_trajectory(tid, succeeded=True)
        except Exception as e:
            errors.append(f"Thread {thread_id} failed: {type(e).__name__}: {e}")

    start_t = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=threads_count) as executor:
        futures = [executor.submit(worker_task, i) for i in range(threads_count)]
        concurrent.futures.wait(futures)
    duration = time.perf_counter() - start_t

    # Analyze output artifacts
    fl_text = collector.florence_jsonl_path.read_text(encoding="utf-8").strip()
    act_text = collector.agent_act_jsonl_path.read_text(encoding="utf-8").strip()

    fl_lines = fl_text.splitlines() if fl_text else []
    act_lines = act_text.splitlines() if act_text else []

    # Verify JSON validity for every single line
    fl_json_errors = 0
    for idx, l in enumerate(fl_lines):
        try:
            d = json.loads(l)
            if "target" not in d or "prefix" not in d:
                fl_json_errors += 1
        except Exception:
            fl_json_errors += 1

    act_json_errors = 0
    for idx, l in enumerate(act_lines):
        try:
            d = json.loads(l)
            if "action" not in d or "trajectory_id" not in d:
                act_json_errors += 1
        except Exception:
            act_json_errors += 1

    # Verify image files existence
    image_files = list(collector.images_dir.glob("*.jpg"))

    # Verify stats.json consistency
    stats = json.loads(collector.stats_path.read_text(encoding="utf-8"))

    # Run dataset validator
    is_valid, val_report = validate_dataset(temp_dir)

    return {
        "threads": threads_count,
        "steps_per_thread": steps_per_thread,
        "total_expected": total_expected,
        "duration_sec": duration,
        "worker_exceptions": errors,
        "florence_line_count": len(fl_lines),
        "act_line_count": len(act_lines),
        "florence_json_errors": fl_json_errors,
        "act_json_errors": act_json_errors,
        "image_count": len(image_files),
        "stats_total_trajectories": stats.get("total_trajectories"),
        "stats_total_steps": stats.get("total_steps"),
        "stats_successful_trajectories": stats.get("successful_trajectories"),
        "dataset_validator_pass": is_valid,
        "dataset_validator_errors": val_report.get("errors", []) if isinstance(val_report, dict) else val_report,
    }


def run_experiment_2_concurrent_harvesters_threadpool(temp_dir: Path) -> dict:
    """Stress tests ThreadPoolExecutor running 15 concurrent agents across Explorer, Web, and Dialog."""
    collector = CPowerDatasetCollector(base_dir=temp_dir, min_free_gb=0.001)
    errors: List[str] = []
    num_each = 5  # 5 explorer, 5 web, 5 dialog = 15 workers, 2 tasks each = 30 tasks total

    start_t = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=15) as executor:
        futures = []
        for i in range(num_each):
            futures.append(executor.submit(worker_explorer_agent, collector, 2))
            futures.append(executor.submit(worker_web_agent, collector, 2))
            futures.append(executor.submit(worker_dialog_agent, collector, 2))

        for f in concurrent.futures.as_completed(futures):
            try:
                f.result(timeout=60)
            except Exception as e:
                errors.append(f"{type(e).__name__}: {e}")
    duration = time.perf_counter() - start_t

    fl_lines = collector.florence_jsonl_path.read_text(encoding="utf-8").strip().splitlines()
    stats = json.loads(collector.stats_path.read_text(encoding="utf-8"))

    return {
        "num_workers": 15,
        "tasks_per_worker": 2,
        "expected_total_samples": 30,
        "duration_sec": duration,
        "errors": errors,
        "florence_line_count": len(fl_lines),
        "stats_total_trajectories": stats.get("total_trajectories"),
        "stats_total_steps": stats.get("total_steps"),
        "stats_successful_trajectories": stats.get("successful_trajectories"),
    }


def run_experiment_3_multi_threaded_keyboard_interrupt(temp_dir: Path) -> dict:
    """
    Stress tests run_continual_cycle_safe when multiple threads have open in-flight trajectories
    and a KeyboardInterrupt is injected.
    """
    collector = CPowerDatasetCollector(base_dir=temp_dir, min_free_gb=0.001)

    def multi_threaded_interrupting_task(col: CPowerDatasetCollector):
        # Start 5 parallel threads that open trajectories
        barrier = threading.Barrier(6)
        traj_ids = []

        def worker(idx: int):
            t = col.start_trajectory(f"ConcurrentInFlight-{idx}")
            traj_ids.append(t)
            col.record_step(
                trajectory_id=t,
                image=Image.new("RGB", (100, 100), (idx * 30, 50, 50)),
                action_type="click",
                phrase=f"إجراء قيد التنفيذ {idx}",
                bbox=(10, 10, 50, 50),
            )
            barrier.wait()  # Synchronize so all 5 are in-flight
            time.sleep(0.5)

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(5)]
        for th in threads:
            th.start()

        barrier.wait()  # Main task waits until all 5 threads have recorded their step
        # Now raise KeyboardInterrupt while 5 trajectories are actively open!
        raise KeyboardInterrupt("Simulated SIGINT while 5 worker threads are in-flight")

    result = run_continual_cycle_safe(collector=collector, task_fn=multi_threaded_interrupting_task)

    stats = json.loads(collector.stats_path.read_text(encoding="utf-8"))

    return {
        "interrupted": result["interrupted"],
        "active_trajectories_remaining": result["active_trajectories_remaining"],
        "stats_file_exists": result["stats_file_exists"],
        "stats_total_trajectories": stats.get("total_trajectories"),
        "stats_successful_trajectories": stats.get("successful_trajectories"),
        "stats_total_steps": stats.get("total_steps"),
    }


if __name__ == "__main__":
    print("=== EMPIRICAL CHALLENGER STRESS HARNESS ===")
    test_dir_root = Path(tempfile.mkdtemp(prefix="cpower_stress_"))
    try:
        print("\n--- Running Experiment 1: 50 Concurrent Threads / 500 Steps Stress Test ---")
        dir1 = test_dir_root / "exp1"
        dir1.mkdir()
        res1 = run_experiment_1_high_concurrency_collector(dir1)
        print(f"Exp 1 Duration: {res1['duration_sec']:.2f}s")
        print(f"Exp 1 Line counts: Florence={res1['florence_line_count']}/{res1['total_expected']}, Act={res1['act_line_count']}/{res1['total_expected']}")
        print(f"Exp 1 JSON errors: Florence={res1['florence_json_errors']}, Act={res1['act_json_errors']}")
        print(f"Exp 1 Stats: traj={res1['stats_total_trajectories']}, steps={res1['stats_total_steps']}, success={res1['stats_successful_trajectories']}")
        print(f"Exp 1 Validator pass: {res1['dataset_validator_pass']}")
        assert res1["florence_line_count"] == 500
        assert res1["act_line_count"] == 500
        assert res1["florence_json_errors"] == 0
        assert res1["act_json_errors"] == 0
        assert res1["stats_total_steps"] == 500
        assert res1["stats_total_trajectories"] == 50
        assert res1["stats_successful_trajectories"] == 50
        assert res1["dataset_validator_pass"] is True
        print(">>> Experiment 1 PASSED: Perfect thread safety, zero data corruption across 50 threads!")

        print("\n--- Running Experiment 2: 15 Concurrent Harvester Workers ThreadPool ---")
        dir2 = test_dir_root / "exp2"
        dir2.mkdir()
        res2 = run_experiment_2_concurrent_harvesters_threadpool(dir2)
        print(f"Exp 2 Duration: {res2['duration_sec']:.2f}s")
        print(f"Exp 2 Lines: {res2['florence_line_count']}/{res2['expected_total_samples']}")
        print(f"Exp 2 Stats: traj={res2['stats_total_trajectories']}, steps={res2['stats_total_steps']}")
        assert res2["florence_line_count"] == 30
        assert res2["stats_total_trajectories"] == 30
        assert len(res2["errors"]) == 0
        print(">>> Experiment 2 PASSED: 15 concurrent workers completed with 0 deadlocks or race conditions!")

        print("\n--- Running Experiment 3: Multi-Threaded KeyboardInterrupt In-Flight Trajectory Cleanup ---")
        dir3 = test_dir_root / "exp3"
        dir3.mkdir()
        res3 = run_experiment_3_multi_threaded_keyboard_interrupt(dir3)
        print(f"Exp 3 Interrupted: {res3['interrupted']}")
        print(f"Exp 3 Active trajectories remaining: {res3['active_trajectories_remaining']}")
        print(f"Exp 3 Stats: total_traj={res3['stats_total_trajectories']}, success={res3['stats_successful_trajectories']}, steps={res3['stats_total_steps']}")
        assert res3["interrupted"] is True
        assert res3["active_trajectories_remaining"] == 0
        assert res3["stats_total_trajectories"] == 5
        assert res3["stats_successful_trajectories"] == 0
        assert res3["stats_total_steps"] == 5
        print(">>> Experiment 3 PASSED: All 5 active in-flight trajectories cleanly closed with succeeded=False!")

        print("\nALL 3 STRESS EXPERIMENTS COMPLETED SUCCESSFULLY!")

    finally:
        shutil.rmtree(test_dir_root, ignore_errors=True)
