/*
 * This file is a part of the open source stm32plus library.
 * Copyright (c) 2011,2012,2013 Andy Brown <www.andybrown.me.uk>
 * Please see website for licensing terms.
 */

#include "config/stm32plus.h"
#include "config/timer.h"
#include "config/timing.h"
#include "config/usart.h"
#include "config/string.h"


using namespace stm32plus;


/**
 * Timer input capture demo.
 *
 * This demonstration will calculate the frequency of a
 * PWM signal and write it out to USART1 every 3 seconds.
 *
 * Note that if you are using the STM32F4DISCOVERY board
 * then you cannot use Usart1 since the pins clash with
 * onboard peripherals. I have tested this code on that
 * board using Uart4.
 *
 * The USART protocol is 57600/8/N/1
 *
 * Timer4 channel 1 is used to generate a PWM
 * signal. This signal is fed to Timer3 channel 3. Each
 * rising edge of the signal causes an interrupt to fire.
 * When two successive edges have been captured we
 * calculate and display the result.
 *
 * On the F4 and F103 HD the frequency is 100KHz. This is too
 * fast for the F100 VL so we use 10KHz instead.
 *
 * You will need to wire PB6 to PB0 to test this demo.
 *
 * Compatible MCU:
 *   STM32F1
 *   STM32F4
 *
 * Tested on devices:
 *   STM32F100RBT6
 *   STM32F103ZET6
 *   STM32F407VGT6
 */

class TimerInputCaptureTest {

  protected:

    /**
     * Declare a type for our input timer.
     */

    typedef Timer3<
        Timer3InternalClockFeature,       // we'll need this for the frequency calculation
        TimerChannel3Feature,             // we're going to use channel 3
        Timer3InterruptFeature,           // we want to use interrupts
        Timer3GpioFeature<                // we want to read something from GPIO
          TIMER_REMAP_NONE,               // the GPIO input will not be remapped
          TIM3_CH3_IN                     // we will read channel 3 from GPIO PB0
        >
      > MyInputTimer;

    /*
     * The timer needs to be a class member so we can see it from the Observable callback
     */

    MyInputTimer *_myInputTimer;

    /*
     * State variables for reading the frequency
     */

    volatile uint16_t _captures[2];
    volatile uint8_t _captureIndex;
    volatile uint32_t _capturedFrequency;
    volatile bool _capturingNextFrequency;

  public:

    void run() {

      /*
       * Declare a USART1 object. Note that an alternative Usart1_Remap object is available
       * if your application demands that you use the alternate pins for USART1
       */

      Usart1<> usart1(57600);

      /*
       * We'll use an output stream for sending to the port instead of using the
       * send(uint8_t) method on the usart object
       */

      UsartPollingOutputStream outputStream(usart1);

      /*
       * We'll use Timer 4 to generate a PWM signal on its channel 1.
       * The signal will be output on PB6
       */

      Timer4<
        Timer4InternalClockFeature,     // clocked from the internal clock
        TimerChannel1Feature,           // we're going to use channel 1
        Timer4GpioFeature<              // we want to output something to GPIO
          TIMER_REMAP_NONE,             // the GPIO output will not (cannot for this timer) be remapped
          TIM4_CH1_OUT                  // we will output channel 1 to GPIO (PB6)
        >
      > outputTimer;

      /*
       * On the F1HD and F4 we set the output timer to 24Mhz with a reload frequency of 100Khz (24Mhz/240).
       * On the F1 VL we set it to 10KHz to avoid CPU starvation by the interrupt handler.
       */

#if defined(STM32PLUS_F1_MD_VL)
      outputTimer.setTimeBaseByFrequency(800000,80-1);
#else
      outputTimer.setTimeBaseByFrequency(24000000,240-1);
#endif

      /*
       * Initialise the output channel for PWM output with a duty cycle of 50%. This will
       * give us a nice square wave for the input capture channel to sample.
       */

      outputTimer.initCompareForPwmOutput(50);

      /*
       * Declare a new instance of the input capture timer.
       */

      _myInputTimer=new MyInputTimer;

      /*
       * Insert our subscribtion of the capture interrupts generated by the input timer
       */

      _myInputTimer->TimerInterruptEventSender.insertSubscriber(
          TimerInterruptEventSourceSlot::bind(this,&TimerInputCaptureTest::onInterrupt)
        );

      /*
       * Initialise the channel (this will be channel 2) for capturing the signal
       */

      _myInputTimer->initCapture(
          TIM_ICPolarity_Rising,      // capture rising edges
          TIM_ICSelection_DirectTI,   // direct connection to timer input trigger
          TIM_ICPSC_DIV1,             // sample every transition
          0,                          // no oversampling filter
          0);                         // prescaler of 0

      /*
       * Reset the variables used to hold the state
       */

      _captureIndex=0;
      _capturingNextFrequency=true;

      /*
       * Enable channel 3 interrupts on Timer 3.
       */

      _myInputTimer->enableInterrupts(TIM_IT_CC3);

      /*
       * Enable both timers to start the action
       */

      outputTimer.enablePeripheral();
      _myInputTimer->enablePeripheral();

      /*
       * Loop until the next frequency has been captured
       */

      for(;;) {

        while(_capturingNextFrequency);

        /*
         * Write out the captured frequency to the USART
         */

        char buf[15];
        StringUtil::modp_uitoa10(_capturedFrequency,buf);
        outputStream << buf << "Hz\r\n";

        /*
         * Pause for 3 seconds
         */

        MillisecondTimer::delay(3000);

        /*
         * start capturing again
         */

        _capturingNextFrequency=true;
      }
    }


    /*
     * Interrupt callback function. This is called when the input capture
     * event is fired
     */

    void onInterrupt(TimerEventType tet,uint8_t /* timerNumber */) {

      if(tet==TimerEventType::EVENT_COMPARE3) {

        // store the current capture time

        _captures[_captureIndex]=_myInputTimer->getCapture();

        if(_captureIndex++==1) {

          // if the main loop is ready then calc the frequency and signal the main loop
          // note the scaling divisor because our timer clocks are scaled by a factor of
          // the APB1 prescaler.

          if(_capturingNextFrequency) {
            _capturedFrequency=_myInputTimer->calculateFrequency(_captures[0],_captures[1]);
            _capturingNextFrequency=false;
          }

          // back to storing at position zero

          _captureIndex=0;
        }
      }
    }
};


/*
 * Main entry point
 */

int main() {

  // we're using interrupts, initialise NVIC

  Nvic::initialise();

  // initialise the SysTick timer

  MillisecondTimer::initialise();

  TimerInputCaptureTest test;
  test.run();

  // not reached
  return 0;
}
