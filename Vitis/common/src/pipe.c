/*
 * Opsero Electronic Design Inc. Copyright 2023
 *
 * The functions in this module allow for initialization of the video pipe and starting of
 * the video pipe.
 */

#include "xstatus.h"
#include "xil_printf.h"
#include "i2c.h"
#include "sleep.h"
#include "ov5640.h"
#include "xv_frmbufrd_l2.h"
#include "xv_frmbufwr_l2.h"
#include "xv_demosaic.h"
#include "xv_gamma_lut.h"
#include "xvprocss_vdma.h"
#include "pipe.h"
#include "math.h"
#include "config.h"

/*
 * Initialize the video pipe
 */
int pipe_init(VideoPipe *pipe, VideoPipeDevIds *devids, XScuGic *intc)
{
	XVprocSs_Config *VprocSsConfigPtr;
	XVidC_VideoStream Stream;
	XVidC_VideoTiming const *TimingPtr;
	int Status;
	XVidC_VideoMode resId;

	/*
	 * Initialize the GPIO driver
	 */
	Status = XGpio_Initialize(&(pipe->Gpio), devids->Gpio);
	if (Status != XST_SUCCESS) {
		xil_printf("Gpio Initialization Failed\r\n");
		return XST_FAILURE;
	}

	// Set GPIO directions (1=input, 0=output)
	XGpio_SetDataDirection(&(pipe->Gpio), 1, ~(GPIO_CAM_IO0_MASK+GPIO_CAM_IO1_MASK));
	// Enable the camera
	XGpio_DiscreteWrite(&(pipe->Gpio), 1, GPIO_CAM_IO0_MASK);

	/*
	 * Initialize the IIC for communication with camera
	 */
	u8 iic_id;
	Status = IicAxiInit(&(pipe->Iic),devids->Iic,intc,devids->IicIntr,&iic_id);
	if (Status != XST_SUCCESS) {
		xil_printf("Failed to initialize the I2C\n\r");
		return XST_FAILURE;
	}

	/*
	 * Initialize the camera
	 * This function will initialize the Camera container and try to communicate with the image sensor
	 * via the I2C bus so that we know what model it is and thus configure the Sensor Demosaic
	 * accordingly.
	 */
	Status = rpi_cam_init(&(pipe->Camera),iic_id,&(pipe->Gpio),GPIO_CAM_IO0_MASK);
	if(Status == XST_FAILURE) {
		pipe->IsConnected = FALSE;
		return(XST_FAILURE);
	}

	/*
	 * Frame Buffer Wr/Rd initialization and config
	 */
	Status = FrmbufWrInit(&(pipe->Frmbuf),devids->FrmbufWr,intc,devids->FrmbufWrIntr,devids->FrmbufBufrBaseAddr);
	if (Status != XST_SUCCESS) {
		xil_printf("Failed to initialize the Frame Buffer Write\n\r");
		return XST_FAILURE;
	}
	Status = FrmbufRdInit(&(pipe->Frmbuf),devids->FrmbufRd,intc,devids->FrmbufRdIntr,devids->FrmbufBufrBaseAddr);
	if (Status != XST_SUCCESS) {
		xil_printf("Failed to initialize the Frame Buffer Read\n\r");
		return XST_FAILURE;
	}

	/*
	 * Demosaic initialization and config
	 */
	XV_demosaic_Initialize(&(pipe->Demosaic), devids->Demosaic);
	XV_demosaic_Set_HwReg_width(&(pipe->Demosaic), VMODE_WIDTH);
	XV_demosaic_Set_HwReg_height(&(pipe->Demosaic), VMODE_HEIGHT);
	XV_demosaic_Set_HwReg_bayer_phase(&(pipe->Demosaic), rpi_cam_bayer_phase(&(pipe->Camera)));
	XV_demosaic_EnableAutoRestart(&(pipe->Demosaic));
	XV_demosaic_Start(&(pipe->Demosaic));

	/*
	 * Gamma LUT initialization and config
	 */
	Status = XV_gamma_lut_Initialize(&(pipe->GammaLut), devids->GammaLut);
	if (Status != XST_SUCCESS) {
		xil_printf("ERROR: Failed to initialize the Gamma LUT\n\r");
		return XST_FAILURE;
	}
	XV_gamma_lut_Set_HwReg_width(&(pipe->GammaLut), VMODE_WIDTH);
	XV_gamma_lut_Set_HwReg_height(&(pipe->GammaLut), VMODE_HEIGHT);
	XV_gamma_lut_Set_HwReg_video_format(&(pipe->GammaLut), 0);
	for(uint32_t i = 0; i < GAMMA_TABLE_SIZE; i++)
	{
		uint16_t value = pow((i / (double)GAMMA_TABLE_SIZE), GAMMA) * (float)GAMMA_TABLE_SIZE;
		Xil_Out16((pipe->GammaLut.Config.BaseAddress + 0x800 + i*2), value );
		Xil_Out16((pipe->GammaLut.Config.BaseAddress + 0x1000 + i*2), value );
		Xil_Out16((pipe->GammaLut.Config.BaseAddress + 0x1800 + i*2), value );
	}
	XV_gamma_lut_Start(&(pipe->GammaLut));
	XV_gamma_lut_EnableAutoRestart(&(pipe->GammaLut));

	/*
	 * Video Processor Subsystem initialization and config
	 */
	VprocSsConfigPtr = XVprocSs_LookupConfig(devids->Vproc);
	if(VprocSsConfigPtr == NULL) {
		xil_printf("ERROR: Video Processor Subsystem device not found\r\n");
		return(XST_FAILURE);
	}
	// Start capturing event log
	XVprocSs_LogReset(&(pipe->Vproc));
	Status = XVprocSs_CfgInitialize(&(pipe->Vproc),
			                        VprocSsConfigPtr,
			                        VprocSsConfigPtr->BaseAddress);
	if(Status != XST_SUCCESS) {
		xil_printf("ERROR: Video Processing Subsystem Init. error\n\r");
		return(XST_FAILURE);
	}

	// Configure the Video Processing Subsystem INPUT stream parameters
	resId = XVidC_GetVideoModeId(VMODE_WIDTH,VMODE_HEIGHT,VMODE_FRAMERATE,FALSE);
	TimingPtr = XVidC_GetTimingInfo(resId);
	Stream.VmId           = resId;
	Stream.Timing         = *TimingPtr;
	Stream.ColorFormatId  = COLOR_FORMAT_ID;
	Stream.ColorDepth     = pipe->Vproc.Config.ColorDepth;
	Stream.PixPerClk      = pipe->Vproc.Config.PixPerClock;
	Stream.FrameRate      = XVidC_GetFrameRate(Stream.VmId);
	Stream.IsInterlaced   = XVidC_IsInterlaced(Stream.VmId);
	XVprocSs_SetVidStreamIn(&(pipe->Vproc), &Stream);

	// Configure the Video Processing Subsystem OUTPUT stream parameters
	resId = XVidC_GetVideoModeId(VPROC_WIDTH_OUT,VPROC_HEIGHT_OUT,VPROC_FRAMERATE_OUT,FALSE);
	TimingPtr = XVidC_GetTimingInfo(resId);
	Stream.VmId           = resId;
	Stream.Timing         = *TimingPtr;
	Stream.ColorFormatId  = COLOR_FORMAT_ID;
	Stream.ColorDepth     = pipe->Vproc.Config.ColorDepth;
	Stream.PixPerClk      = pipe->Vproc.Config.PixPerClock;
	Stream.FrameRate      = XVidC_GetFrameRate(Stream.VmId);
	Stream.IsInterlaced   = XVidC_IsInterlaced(Stream.VmId);
	XVprocSs_SetVidStreamOut(&(pipe->Vproc), &Stream);

	// Start the Video Processor Subsystem
	Status = XVprocSs_SetSubsystemConfig(&(pipe->Vproc));

	pipe->IsConnected = TRUE;

	return(XST_SUCCESS);
}

int pipe_start_camera(VideoPipe *pipe)
{
	int Status;

	// Start the RPi camera
	Status = rpi_cam_config(&(pipe->Camera));
	// Start the Frame buffers
	Status = FrmbufStart(&(pipe->Frmbuf));
	return(Status);
}

