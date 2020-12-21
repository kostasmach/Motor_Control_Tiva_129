//*****************************************************************************
//
// Motor Control with Tiva
// Open a terminal with 115,200 8-N-1 to control
//
//*****************************************************************************
//
// Connections
//
// PL4 -> DIR Motor
// PG0 -> PWM Motor
//
// PL1 -> PhA
// PL2 -> PhB
//
//*****************************************************************************


#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "inc/tm4c1294ncpdt.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_gpio.h"
//#include "inc/hw_qei.h"
#include "driverlib/sysctl.h"
#include "driverlib/fpu.h"
#include "driverlib/gpio.h"
#include "driverlib/debug.h"
#include "driverlib/pwm.h"
#include "driverlib/pin_map.h"
#include "driverlib/qei.h"
#include "driverlib/timer.h"
#include "driverlib/uart.h"
#include "driverlib/interrupt.h"
#include "utils/uartstdio.h"

#include "driverlib/systick.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "inc/hw_ints.h"
#include "driverlib/debug.h"
#include "driverlib/ssi.h"
#include "utils/ustdlib.h"
#include "utils/cmdline.h"
#include "fatfs/src/ff.h"
#include "fatfs/src/diskio.h"


//*****************************************************************************
//
// SD
//
//*****************************************************************************
/*Variable required for SD Card R/W*/
static FATFS g_sFatFs; // The following are data structures used by FatFs.
FRESULT iFResult;
FIL fil;
uint32_t count;
uint32_t counter_sd = 0;


//*****************************************************************************
//
// Global Variables
//
//*****************************************************************************
uint32_t g_ui32SysClock;			// System clock frequency
int Step = 0;		              	// Input from user
uint32_t Setpoint = 2000000000;		// Setpoint for motor command
volatile int32_t Direction;    		// Motor Direction
volatile int32_t Velocity;     		// Motor Velocity [counts/period]
volatile uint32_t Position;    		// Motor Position [counts] 
int32_t error = 0;             		// Control error [Counts]
double Kp = 0.0002;            		// Kp gain for position control
double u1 = 0;                  		// Output command(%)
#define UpLimit 15              	// Maximum PWM output value


//*****************************************************************************
//
// Configure the UART and its pins
//
//*****************************************************************************
void ConfigureUART(void)
{
    //
    // Enable the GPIO Peripheral used by the UART
    //
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);

	//
    // Enable UART0
    //
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
	
	//
    // Configure GPIO Pins for UART mode
    //
    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);
    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);
	
    //
    // Initialize the UART for console I/O
    //
	UARTStdioConfig(0, 115200, g_ui32SysClock);
}


//*****************************************************************************
//
// Timer 0 configuration
//
//*****************************************************************************
void ConfigureTimer0(void)
{
    //
    // Timer frequency 10 KHz
    //
    uint32_t ui32Period = g_ui32SysClock / 10000;

    //
    // Before calling any peripheral specific driverLib function
    // enable the clock to that peripheral
    //
    SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);

    //
    // Configure Timer 0 as a 32-bit timer in periodic mode
    //
    TimerConfigure(TIMER0_BASE, TIMER_CFG_PERIODIC);

    //
    // Load the desired period into the Timer’s Interval Load register
    //
    TimerLoadSet(TIMER0_BASE, TIMER_A, ui32Period -1);

    //
    // Enable the interrupt not only in the timer module, but also in the NVIC
    // (the Nested Vector Interrupt Controller, the Cortex M4’s interrupt controller)
    //
    // -> IntMasterEnable() is the master interrupt enable API for all interrupts
    // -> IntEnable() enables the specific vector associated with Timer0A
    // -> TimerIntEnable() enables a specific event within the timer to generate an interrupt
    //
    // In this case we are enabling an interrupt to be generated on a timeout of Timer 0A
    //
    IntEnable(INT_TIMER0A);
    TimerIntEnable(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
    IntMasterEnable();

    //
    // Enable the timer - This will start the timer and interrupts
    // will begin triggering on the timeouts
    //
    TimerEnable(TIMER0_BASE, TIMER_A);
}


//*****************************************************************************
//
// Configure PWM Pin for motor control
//
//*****************************************************************************
void ConfigureMotorPWM(void)
{
    //
    // Configure PWM Clock to match system's clock
    //
    SysCtlPWMClockSet(SYSCTL_PWMDIV_1);
    SysCtlDelay(10);	
	
    //
    // Enable the PWM0 peripheral
    //
    SysCtlPeripheralEnable(SYSCTL_PERIPH_PWM0);
	
    //
    // Enable the GPIO port G for the output PWM signal
    //
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOG);
    SysCtlDelay(10);
	
    //
    // Pin PG0 as PWM (M0PWM4)
    //
	GPIOPinConfigure(GPIO_PG0_M0PWM4);
	
    //
    // Pin PG0 as output PWM
    //
	GPIOPinTypePWM(GPIO_PORTG_BASE, GPIO_PIN_0);
	
	//
    // Configure the PWM generator
    //
	PWMGenConfigure(PWM0_BASE, PWM_GEN_2, PWM_GEN_MODE_UP_DOWN | PWM_GEN_MODE_NO_SYNC);
    SysCtlDelay(10);
	
	//
    // Set the PWM period
    // Desired PWM frequency: 20KHz -> Period: 1/20.000s = 50us
	// N = (1 / f) * SysClk.  Where N [cycles] is the function parameter, 
	// f is the desired frequency, and SysClk is the system clock frequency.
    // In this case: (1 / 20KHz) * 120MHz = 6000 cycles.
    //
	PWMGenPeriodSet(PWM0_BASE, PWM_GEN_2, 6000);
    SysCtlDelay(10);
}


//*****************************************************************************
//
// Configure direction pin for motor control
//
//*****************************************************************************
void ConfigureMotorDirectionPin(void)
{
    //
    // Enable the port L for the output direction signal
    //
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOL);

    //
    // Pin PL4 Direction for Motor
    //
    GPIOPinTypeGPIOOutput(GPIO_PORTL_BASE, GPIO_PIN_4);
	
	//GPIOPadConfigSet(GPIO_PORTL_BASE, GPIO_PIN_4, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD_WPD);
    //GPIOPinWrite(GPIO_PORTL_BASE, GPIO_PIN_4, 0);
}


//*****************************************************************************
//
// Configure QEI
//
//*****************************************************************************
void ConfigureQEI(void)
{
    //
    // Enable Peripheral port L for the encoder
    //
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOL);
	
    //
    // Enable QEI0 Peripheral
    //
    SysCtlPeripheralEnable(SYSCTL_PERIPH_QEI0);
	SysCtlDelay(10);
	
	//
    // Pin PL1 as PhA0
	// Pin PL2 as PhB0
    //
	GPIOPinConfigure(GPIO_PL1_PHA0); 
    GPIOPinConfigure(GPIO_PL2_PHB0); 
    GPIOPinTypeQEI(GPIO_PORTL_BASE, GPIO_PIN_1 | GPIO_PIN_2);
	
	//
    // Disable QEI0
    //
    QEIDisable(QEI0_BASE);
	
    //
    // Disable QEI0 velocity capture
    //
    QEIVelocityDisable(QEI0_BASE);
	
    //
    // Disable QEI0 interrupt sources
    //
    QEIIntDisable(QEI0_BASE, (QEI_INTERROR | QEI_INTDIR | QEI_INTTIMER | QEI_INTINDEX));

	//
    // Configure QEI0
    //
    // capture on both A and B
    // do not reset when there is an index pulse
    // do not swap signals PHA0 and PHB0
    // set the maximum position as 4294967200, since max value for uint32_t is 4294967295
    //
    QEIConfigure(QEI0_BASE, (QEI_CONFIG_CAPTURE_A_B | QEI_CONFIG_NO_RESET | QEI_CONFIG_QUADRATURE | QEI_CONFIG_NO_SWAP), 4294967200);  
	SysCtlDelay(10);
	
    //
    // Configure the velocity capture for QEI0
    // 40000 is the period at which the velocity will be measured
    //
    QEIVelocityConfigure(QEI0_BASE, QEI_VELDIV_16, 40000);
    SysCtlDelay(10);
	
	//
    // Enable QEI0
    //
    QEIEnable(QEI0_BASE);
    SysCtlDelay(10);

    //
    // Set the current position for QEI0 somewhere in the
    // middle of the uint32_t range [0, 4294967295]
    //
    QEIPositionSet(QEI0_BASE, 2000000000);
    SysCtlDelay(10);
	
	//
    // Enable velocity capture for QEI0
    //
    QEIVelocityEnable(QEI0_BASE);
}


//*****************************************************************************
//
// Drive Motor
//
//*****************************************************************************
void DriveMotor(int8_t u)
{
    //
    // If duty cycle is 0 , disable the PWM generator and output
    //
    if(!u)
	{	
		PWMOutputState(PWM0_BASE, PWM_OUT_4_BIT, false);
        PWMGenDisable(PWM0_BASE, PWM_GEN_2);
	}
	//
	// else set direction and PWM output accordingly
	//
	else
	{
		//
		// Set direction
		//
		if (u < 0)
		{
			u = -u;
			GPIOPinWrite(GPIO_PORTL_BASE, GPIO_PIN_4, 0);
		}
		else
		{
            GPIOPinWrite(GPIO_PORTL_BASE, GPIO_PIN_4, GPIO_PIN_4);
		}

		//
		// Set motor duty cycle
		//		
	    PWMPulseWidthSet(PWM0_BASE, PWM_OUT_4, PWMGenPeriodGet(PWM0_BASE, PWM_GEN_2) / 100 * (uint32_t)u);
        PWMOutputState(PWM0_BASE, PWM_OUT_4_BIT, true);
        PWMGenEnable(PWM0_BASE, PWM_GEN_2);
	}
}

//*****************************************************************************
//
// log
//
//*****************************************************************************
void logger(void)
{
    char buffer[13];
    sprintf(buffer, "%12d\n", Position);
    iFResult =  f_write(&fil, buffer, 13, &count);
    iFResult = f_sync(&fil); //call everytime you need to update file
}



//*****************************************************************************
//
// Position Control
//
//*****************************************************************************
void PositionControl(void)
{
    //
    // Read encoder Position, Velocity and Direction
    // Get direction (1 = forward, -1 = backward)
    // Get velocity (counts per period) and multiply by direction so that it is signed
    //
    Position = QEIPositionGet(QEI0_BASE);
    Direction = QEIDirectionGet(QEI0_BASE);
    Velocity = QEIVelocityGet(QEI0_BASE) * Direction;

    //
    // Control Algorithm
    //
    error = Setpoint - Position;
    u1 = - Kp * error;
	
	//
    // Apply saturation limits
    //
    if (u1 > UpLimit)
        u1 = UpLimit;

	//
	// Drive Motor 1
	//	
	DriveMotor((int8_t)u1);

    //
    // log
    //
	if(counter_sd++ == 1000)
	{
        logger();
        counter_sd = 0;
	}


}



//*****************************************************************************
//
// Timer 0 handler
//
//*****************************************************************************
void Timer0IntHandler(void)
{
    //
    // Clear the timer interrupt
    //
    TimerIntClear(TIMER0_BASE, TIMER_TIMA_TIMEOUT);

	//
    // Planning
    //
    Setpoint += Step;
	
    //
    // Control
    //
    PositionControl();
}


//*****************************************************************************
//
// Main function
//
//*****************************************************************************
int main(void)
{
	//
	// Variables
	//
	unsigned char user_input[8];  		// Buffer for storing keyboard input
	// INITIALIZE BUFFER -------------------------------------------------------------------------------
	int user_input_dec = 0;				// Decimal value after conversion
	
    //
    // Run clock at 120 MHz
    //
    g_ui32SysClock = SysCtlClockFreqSet((SYSCTL_XTAL_25MHZ | SYSCTL_OSC_MAIN |
            SYSCTL_USE_PLL | SYSCTL_CFG_VCO_480), 120000000);
	
    //
    // Systick
    //
    SysTickPeriodSet(g_ui32SysClock / 100);
    SysTickEnable();
    SysTickIntEnable();
    IntMasterEnable();

    //
    // Configure PWM Pins, Direction Pins and QEI for Motor 1
    //
    ConfigureMotorPWM();
    ConfigureMotorDirectionPin();
    ConfigureQEI();

    //
    // Initialize the UART and say hello
    //
    ConfigureUART();
    UARTprintf("\nHi!\n");

    //
    // Configure Timer 0
    //
    ConfigureTimer0();



    // mount the file system, using logical disk 0.
    iFResult = f_mount(0, &g_sFatFs);

    if(iFResult != FR_OK)
    {
        UARTprintf("f_mount error\n");
        return 0;
    }
    else
    {
        UARTprintf("f_mount success!\n");
    }

    uint32_t count;
    iFResult = f_open(&fil, "log3.txt", FA_CREATE_ALWAYS|FA_WRITE); //warning these flags will overwrite existing files!! if you dont want that to happen use:FA_CREATE_NEW|FA_WRITE
    UARTprintf("f_open success!\n");
    iFResult =  f_write(&fil, "Hello!", 6, &count);

    //
    // Main loop
    //
    while(1)
    {
        //
        // Ask for input
        //
        UARTprintf("Give command\n");

        //
        // Read < 8 bytes and hit enter
        //
        UARTgets(user_input,8);

        //
        // Convert input to decimal
        //
        sscanf(user_input,"%d",&user_input_dec);

        //
        // Filter Input
        //
        if ((user_input_dec > 11) || (user_input_dec < -11))
        {
            //
            // INVALID INPUT - Do not update motor command
            //
            UARTprintf("Desired step: %d\n", user_input_dec);
            UARTprintf("INVALID INPUT\n");

            iFResult = f_close(&fil); //when you finish
            UARTprintf("f_close success!\n");
            f_mount(0, NULL);
            UARTprintf("unmount success!\n");
        }
        else
        {
            //
            // Update motor command - Change ang. rate
            //
            Step = user_input_dec;
					
			//
			// UART print useful info
			//
            UARTprintf("Desired step: %d\n", Step);
            UARTprintf("Position 0: %d\n", Position);
            UARTprintf("Desired Setpoint: %d\n", Setpoint);
            UARTprintf("Error: %d\n", error);
            UARTprintf("Output: %d\n", (int)u1);
        }
        SysCtlDelay(10);
    }
}
