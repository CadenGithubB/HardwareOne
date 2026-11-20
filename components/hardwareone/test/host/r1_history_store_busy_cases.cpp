// S-5 cases for the R1 history store, appended by test_r1_history_store.py to
// r1_history_store_harness.cpp (whose main becomes harnessMain and runs
// first). They use that harness's disk, clock, fixtures and model helpers, and
// the runner's seam that lets another task hold the filesystem lock
// (fsHeldElsewhere, fsTryMisses, fsBlockingWaitsWhileBusy).
#if !BASELINE
static void busyCommitWaitsForALaterLap() {
  reset(); auto day = fixture(); const auto path = primary(day);
  check(r1HealthHistoryStoreStage(day, 1), "stage accepted");
  fsHeldElsewhere = true; fsTryMisses = fsBlockingWaitsWhileBusy = 0;
  for (int lap = 0; lap < 5; ++lap) { r1HealthHistoryStoreTick(); released(); nowMs += kLapMs; }
  check(operations.empty() && disk.empty(), "a busy filesystem lock starts no commit: nothing opened, nothing written");
  check(fsTryMisses == 5 && fsBlockingWaitsWhileBusy == 0, "every lap tries the lock and none waits for it");
  auto st = status();
  check(st.commitPending && st.stagedGeneration == 1 && st.committedGeneration == 0, "the staged day stays staged");
  check(st.error == R1_HISTORY_STORE_OK && st.fsBusyDeferrals == 5, "a deferral is counted and is not an error");
  check(sNextRetryMs == 0 && sRetryDelayMs == kRetryInitialMs, "a deferral adds no retry delay");
  // The lock is free: the very next lap commits, once.
  fsHeldElsewhere = false;
  r1HealthHistoryStoreTick(); released();
  good(day, 1);
  check(disk.count(path) == 1 && status().fsBusyDeferrals == 5 && fsBlockingWaitsWhileBusy == 0, "the kept day is committed after release");
  const auto bytes = disk.at(path); const size_t writesBefore = operations.size();
  r1HealthHistoryStoreTick(); released();
  check(disk.at(path) == bytes && operations.size() == writesBefore, "and committed exactly once");

  // A newer generation staged while the lock was busy is the one committed.
  reset(); day = fixture(); auto newer = day; ++newer.activity.slots[0].steps;
  check(r1HealthHistoryStoreStage(day, 1), "stage accepted");
  fsHeldElsewhere = true; r1HealthHistoryStoreTick(); released();
  check(r1HealthHistoryStoreStage(newer, 2), "a newer generation stages while the commit waits");
  r1HealthHistoryStoreTick(); released();
  fsHeldElsewhere = false; r1HealthHistoryStoreTick(); released();
  good(newer, 2);

  // The task's own hold is not "busy".
  reset(); day = fixture(); {
    FsLockGuard outer("test.caller"); check(r1HealthHistoryStoreStage(day, 1), "stage with outer guard");
    r1HealthHistoryStoreTick();
    check(gFsMutex->held && status().fsBusyDeferrals == 0, "a tick under the caller's own guard commits");
  } released(); good(day, 1);
}

static void busyLoadIsNeitherAbsentNorFailed() {
  reset(); const auto day = fixture(); const auto path = primary(day); save(day, 1);
  const auto stored = disk.at(path);
  R1HealthHistoryDay loaded; operations.clear();
  fsHeldElsewhere = true; fsBlockingWaitsWhileBusy = 0;
  check(r1HealthHistoryStoreLoadExact(day.peerId, day.dayStart, day.timezoneMinutes, loaded) == R1_HISTORY_STORE_BUSY,
        "a load while the filesystem is busy answers BUSY, not NOT_FOUND and not a read failure");
  released();
  check(operations.empty() && fsBlockingWaitsWhileBusy == 0, "the busy load touches nothing and does not wait");
  check(status().error == R1_HISTORY_STORE_OK && status().fsBusyDeferrals == 1, "BUSY is counted and never becomes the store's error");
  check(std::string(r1HealthHistoryStoreErrorName(R1_HISTORY_STORE_BUSY)) == "busy", "BUSY has its own name");
  check(loaded.dayStart == 0, "the output day is cleared");
  fsHeldElsewhere = false;
  check(r1HealthHistoryStoreLoadExact(day.peerId, day.dayStart, day.timezoneMinutes, loaded) == R1_HISTORY_STORE_OK &&
        std::memcmp(&loaded, &day, sizeof(day)) == 0, "the day loads once the lock is free");
  released();

  // The model: more busy laps than load failures it tolerates. The stored day
  // is neither given up nor replaced by the sweep's sparse page.
  reset(); save(day, 1); modelReboot(day.peerId);
  check(sweep(day.dayStart, 5), "the model accepts a sweep page for the stored day");
  fsHeldElsewhere = true; fsBlockingWaitsWhileBusy = 0;
  mainLoop(kHistoryLoadMaxAttempts + 8);
  check(fsBlockingWaitsWhileBusy == 0, "no lap of the health model waits for the filesystem");
  check(disk.at(path) == stored, "the stored day is untouched while the filesystem is busy");
  check(sPendingHistory.keyKnown && sPendingHistory.loadFailures == 0 && sPendingHistory.loadRetryAtMs == 0,
        "a busy load is not counted as a load failure and adds no retry delay");
  check(sHistoryUiFetchState != R1_HISTORY_ERROR && !sDropCurrentHistoryFetch, "no error is shown and the sweep is not dropped");
  fsHeldElsewhere = false;
  mainLoop(3);
  R1HealthHistoryDay merged;
  check(r1HealthHistoryStoreLoadExact(day.peerId, day.dayStart, day.timezoneMinutes, merged) == R1_HISTORY_STORE_OK, "the swept day is committed");
  released();
  check(merged.heartRate.count == 2 && merged.heartRate.slots[0].valid && merged.heartRate.slots[5].valid &&
        merged.hrv.have && merged.spo2.have && merged.activity.have,
        "after the busy laps the sweep is merged into the stored day, not written over it");
}
#endif

int main() {
  const int harness = harnessMain();
  if (harness != 0) return harness;
#if !BASELINE
  const size_t before = checks;
  busyCommitWaitsForALaterLap();
  busyLoadIsNeitherAbsentNorFailed();
  fsHeldElsewhere = false;
  std::cerr << "filesystem-busy cases: " << (checks - before) << " assertions passed\n";
#endif
  return 0;
}
