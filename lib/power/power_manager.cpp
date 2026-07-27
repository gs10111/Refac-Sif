#include "power_manager.h"

#define ACQUISITION_CPU_MHZ 10
#define TRANSMIT_CPU_MHZ 240
#define TRANSMIT_SETTLE_MS 100
#define EXT0_WAKE_ON_LOW 0

PowerManager::PowerManager(IImu &imu, IRadio &radio, ICpu &cpu, ISleeper &sleeper)
    : _imu(imu),
      _radio(radio),
      _cpu(cpu),
      _sleeper(sleeper)
{
}

void PowerManager::deepSleepTimer(uint16_t minutes)
{
    _radio.off();
    _imu.sleep();
    _sleeper.enableTimerWakeup((uint64_t)minutes * 60ULL * 1000000ULL);
    _sleeper.start();
}

void PowerManager::deepSleepUntilTrigger(uint8_t triggerPin)
{
    _radio.off();

#ifdef MUTANT_IMU_STAYS_AWAKE_ON_TRIGGER_SLEEP
    // ==== MUTATION: MUTANT_IMU_STAYS_AWAKE_ON_TRIGGER_SLEEP ====
    // Built only by [env:mutant_imu_stays_awake_on_trigger_sleep].
    // Never by env:native or env:pico32.
    // Run it:  pio test -e mutant_imu_stays_awake_on_trigger_sleep
    //
    // BREAKS: imu.sleep() is skipped on THIS path only. The timer path still
    //         sleeps it.
    // WHY:    the tempting version of R5. Someone reasons that the ext0 wait is
    //         the "short" sleep and the IMU can stay up through it — but under D1
    //         this is the wait that lasts hours with the belt idle, so it is the
    //         more expensive of the two to get wrong. The symptom is a battery
    //         flat in weeks instead of years and it appears in no functional test.
    // CAUGHT BY: test_power test_trigger_sleep_turns_the_radio_off_and_sleeps_the_imu_first
    // SURVIVED BY: the timer-path test, deliberately. Skipping imu.sleep()
    //         everywhere would kill both and prove only that at least one works;
    //         breaking a single path proves the two are independently pinned.
#else
    _imu.sleep();
#endif

    _sleeper.enableExt0Wakeup(triggerPin, EXT0_WAKE_ON_LOW);
    _sleeper.start();
}

void PowerManager::enterAcquisitionMode()
{
    _radio.off();
    _cpu.setFrequencyMhz(ACQUISITION_CPU_MHZ);
}

void PowerManager::enterTransmitMode()
{
    _cpu.setFrequencyMhz(TRANSMIT_CPU_MHZ);
    _cpu.delayMs(TRANSMIT_SETTLE_MS);
}
