"""
CPower Multi-Agent Continual Harvesting & Training Orchestrator (clients/python/cpower_continual_loop.py).

Orchestrates multi-agent task generation and incremental local training:
  1. Worker A: Explorer & File System Harvester.
  2. Worker B: Web & Media Interaction Harvester.
  3. Worker C: Dialog & Control Action Harvester.
  4. Worker D: Arabic RTL Layout Synthesizer.
  5. Orchestrator: ContinualTrainingLoop with disk space guards and safe lifecycle management.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import sys
import time
from typing import Any, Callable, Dict, List, Optional, Union

CLIENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CLIENT_DIR.parent.parent
sys.path.insert(0, str(CLIENT_DIR))

from cpower_dataset_collector import CPowerDatasetCollector  # noqa: E402
from cpower_multitask_harvester import MultiTaskUIHarvester  # noqa: E402
from cpower_trainer import train_local  # noqa: E402


def worker_explorer_agent(collector: CPowerDatasetCollector, count: int) -> int:
    harvester = MultiTaskUIHarvester(collector)
    for i in range(1, count + 1):
        traj_id = collector.start_trajectory(f"SubAgent-Explorer: Navigation Task #{i}")
        harvester.generate_explorer_task(traj_id, step_idx=1)
        collector.complete_trajectory(traj_id, succeeded=True)
    return count


def worker_web_agent(collector: CPowerDatasetCollector, count: int) -> int:
    harvester = MultiTaskUIHarvester(collector)
    for i in range(1, count + 1):
        traj_id = collector.start_trajectory(f"SubAgent-Web: Media & Search Task #{i}")
        harvester.generate_youtube_task(traj_id, step_idx=1)
        collector.complete_trajectory(traj_id, succeeded=True)
    return count


def worker_dialog_agent(collector: CPowerDatasetCollector, count: int) -> int:
    harvester = MultiTaskUIHarvester(collector)
    for i in range(1, count + 1):
        traj_id = collector.start_trajectory(f"SubAgent-Dialog: System Control Task #{i}")
        harvester.generate_system_dialog_task(traj_id, step_idx=1)
        collector.complete_trajectory(traj_id, succeeded=True)
    return count


def worker_arabic_agent(collector: CPowerDatasetCollector, count: int) -> int:
    harvester = MultiTaskUIHarvester(collector)
    for i in range(1, count + 1):
        traj_id = collector.start_trajectory(f"SubAgent-Arabic: RTL UI Task #{i}")
        harvester.generate_arabic_rtl_task(traj_id, step_idx=1)
        collector.complete_trajectory(traj_id, succeeded=True)
    return count


class ContinualTrainingLoop:
    """
    CPower Multi-Agent Continual Harvesting & Training Orchestrator.
    Manages continuous lifecycle of concurrent multi-domain harvesters
    interleaved with parameter-efficient fine-tuning on Microsoft Florence-2.
    """

    def __init__(
        self,
        collector: Optional[CPowerDatasetCollector] = None,
        base_dir: Optional[Union[str, Path]] = None,
        samples_per_worker: int = 5,
        train_steps: int = 5,
        epochs: int = 1,
        lr: float = 1e-4,
        device: str = "cpu",
        val_ratio: float = 0.1,
        min_free_gb: float = 1.0,
        enable_arabic_worker: bool = True,
    ):
        self.collector = collector or CPowerDatasetCollector(base_dir=base_dir, min_free_gb=min_free_gb)
        self.base_dir = self.collector.base_dir
        self.samples_per_worker = samples_per_worker
        self.train_steps = train_steps
        self.epochs = epochs
        self.lr = lr
        self.device = device
        self.val_ratio = val_ratio
        self.min_free_gb = min_free_gb
        self.enable_arabic_worker = enable_arabic_worker

        self._is_running = False
        self.cycle_count = 0
        self.total_samples_harvested = 0
        self.latest_metrics: Dict[str, Any] = {}
        self.cycle_history: List[Dict[str, Any]] = []

    def run_harvest_step(self, samples_per_worker: Optional[int] = None) -> Dict[str, Any]:
        count = samples_per_worker if samples_per_worker is not None else self.samples_per_worker
        workers = [
            (worker_explorer_agent, count),
            (worker_web_agent, count),
            (worker_dialog_agent, count),
        ]
        if self.enable_arabic_worker:
            workers.append((worker_arabic_agent, count))

        results = {}
        with ThreadPoolExecutor(max_workers=len(workers)) as executor:
            futures = {executor.submit(fn, self.collector, cnt): fn.__name__ for fn, cnt in workers}
            for fut in futures:
                name = futures[fut]
                results[name] = fut.result()

        harvested = sum(results.values())
        self.total_samples_harvested += harvested
        return {"harvested": harvested, "by_worker": results}

    def run_training_step(self, train_steps: Optional[int] = None) -> Dict[str, Any]:
        steps = train_steps if train_steps is not None else self.train_steps
        metrics = train_local(
            dataset_root=self.collector.base_dir,
            epochs=self.epochs,
            lr=self.lr,
            device_name=self.device,
            max_steps=steps,
            val_ratio=self.val_ratio,
        )
        self.latest_metrics = metrics if isinstance(metrics, dict) else {"checkpoint_dir": metrics}
        return self.latest_metrics

    def run_single_cycle(
        self,
        samples_per_worker: Optional[int] = None,
        train_steps: Optional[int] = None,
    ) -> Dict[str, Any]:
        self.cycle_count += 1
        t0 = time.time()
        harvest_res = self.run_harvest_step(samples_per_worker=samples_per_worker)
        train_res = self.run_training_step(train_steps=train_steps)
        elapsed = time.time() - t0

        summary = {
            "cycle": self.cycle_count,
            "elapsed_seconds": round(elapsed, 2),
            "samples_harvested": harvest_res["harvested"],
            "harvest_details": harvest_res,
            "training_metrics": train_res,
        }
        self.cycle_history.append(summary)
        return summary

    def run_continual(
        self,
        max_cycles: Optional[int] = None,
        cycle_delay: float = 0.0,
    ) -> Dict[str, Any]:
        self._is_running = True
        try:
            while self._is_running:
                if max_cycles is not None and self.cycle_count >= max_cycles:
                    break
                if not self.collector._check_disk_space():
                    print(f"[CONTINUAL_LOOP] Disk space below safety threshold on {self.collector.base_dir}. Halting loop.")
                    break
                try:
                    self.run_single_cycle()
                except IOError as e:
                    print(f"[CONTINUAL_LOOP] Cycle stopped due to I/O or disk space limit: {e}")
                    break
                if cycle_delay > 0 and self._is_running:
                    time.sleep(cycle_delay)
        except KeyboardInterrupt:
            print("[CONTINUAL_LOOP] Interrupted by user.")
        finally:
            self.cleanup()

        return {
            "cycles_completed": self.cycle_count,
            "total_harvested": self.total_samples_harvested,
            "latest_metrics": self.latest_metrics,
            "stopped_cleanly": True,
        }

    def stop(self) -> None:
        self._is_running = False

    def cleanup(self) -> None:
        # Finalize dangling active trajectories if interrupted or stopped
        for tid in list(self.collector._active_trajectories.keys()):
            self.collector.complete_trajectory(tid, succeeded=False, notes="Continual loop stopped/interrupted")


def run_multi_agent_continual_cycle(
    samples_per_worker: int = 15,
    train_steps: int = 5,
    enable_arabic_worker: bool = False,
) -> None:
    collector = CPowerDatasetCollector()
    print("=" * 70)
    print(">>> CPower Multi-Agent Continual Learning & Training Cycle <<<")
    print(f"Target Storage: {collector.base_dir}")
    print("=" * 70)

    workers = [
        (worker_explorer_agent, samples_per_worker),
        (worker_web_agent, samples_per_worker),
        (worker_dialog_agent, samples_per_worker),
    ]
    if enable_arabic_worker:
        workers.append((worker_arabic_agent, samples_per_worker))

    worker_count = len(workers)
    print(f"[ORCHESTRATOR] Launching {worker_count} subagent workers ({samples_per_worker} tasks each)...")
    start_harvest = time.time()

    with ThreadPoolExecutor(max_workers=worker_count) as executor:
        futures = [executor.submit(fn, collector, cnt) for fn, cnt in workers]
        counts = [f.result() for f in futures]

    total_new = sum(counts)
    harvest_elapsed = time.time() - start_harvest
    print(f"[ORCHESTRATOR] All {worker_count} workers completed: {total_new} new multi-domain samples harvested in {harvest_elapsed:.2f}s!")

    summary = collector.get_dataset_summary()
    print(f"[ORCHESTRATOR] Dataset Pool: {summary['florence_samples']} total grounding samples available.")

    # 2. Local Fine-Tuning Pass
    print(f"[ORCHESTRATOR] Launching incremental local training ({train_steps} steps)...")
    train_res = train_local(
        dataset_root=collector.base_dir,
        epochs=1,
        lr=1e-4,
        device_name="cpu",
        max_steps=train_steps,
    )
    checkpoint_dir = train_res["checkpoint_dir"] if isinstance(train_res, dict) else train_res
    print(f"[ORCHESTRATOR] Training cycle complete! Checkpoint updated at: {checkpoint_dir}")


def run_continual_cycle_safe(
    collector: Optional[CPowerDatasetCollector] = None,
    task_fn: Optional[Callable[[CPowerDatasetCollector], Any]] = None,
    samples_per_worker: int = 1,
    train_steps: int = 0,
) -> Dict[str, Any]:
    """
    Executes continual learning cycle with graceful KeyboardInterrupt (SIGINT) protection.
    Catches KeyboardInterrupt, completes all pending in-flight trajectories, and flushes stats.
    """
    if collector is None:
        collector = CPowerDatasetCollector()

    interrupted = False
    try:
        if task_fn is not None:
            task_fn(collector)
        else:
            run_multi_agent_continual_cycle(samples_per_worker=samples_per_worker, train_steps=train_steps)
    except KeyboardInterrupt:
        interrupted = True
        # Gracefully finalize all active uncompleted trajectories
        for tid in list(collector._active_trajectories.keys()):
            collector.complete_trajectory(tid, succeeded=False, notes="Graceful shutdown on KeyboardInterrupt")

    return {
        "interrupted": interrupted,
        "active_trajectories_remaining": len(collector._active_trajectories),
        "stats_file_exists": collector.stats_path.is_file(),
    }


__all__ = [
    "worker_explorer_agent",
    "worker_web_agent",
    "worker_dialog_agent",
    "worker_arabic_agent",
    "ContinualTrainingLoop",
    "run_multi_agent_continual_cycle",
    "run_continual_cycle_safe",
]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="CPower Continual Learning Cycle")
    parser.add_argument("--samples-per-worker", type=int, default=15, help="Samples per worker")
    parser.add_argument("--train-steps", type=int, default=5, help="Training steps")
    parser.add_argument("--enable-arabic", action="store_true", help="Enable Arabic RTL worker")
    args = parser.parse_args()

    run_multi_agent_continual_cycle(
        samples_per_worker=args.samples_per_worker,
        train_steps=args.train_steps,
        enable_arabic_worker=args.enable_arabic,
    )
