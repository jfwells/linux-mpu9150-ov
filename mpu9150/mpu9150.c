////////////////////////////////////////////////////////////////////////////
//
//  This file is part of linux-mpu9150
//
//  Copyright (c) 2013 Pansenti, LLC
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of 
//  this software and associated documentation files (the "Software"), to deal in 
//  the Software without restriction, including without limitation the rights to use, 
//  copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the 
//  Software, and to permit persons to whom the Software is furnished to do so, 
//  subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all 
//  copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, 
//  INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A 
//  PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
//  HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION 
//  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <stdio.h>
#include <string.h>

#include "linux_glue.h"
#include "inv_mpu.h"
#include "inv_mpu_dmp_motion_driver.h"
#include "mpu9150.h"
#include "../ahrs_settings.h"

static int data_ready();
static void calibrate_data(mpudata_t *mpu);
static void tilt_compensate(quaternion_t magQ, quaternion_t unfusedQ);
static int data_fusion(mpudata_t *mpu);
static unsigned short inv_row_2_scale(const signed char *row);
static unsigned short inv_orientation_matrix_to_scalar(signed char *mtx);

extern int g_debug;

int debug_on;
int yaw_mixing_factor;

int use_accel_cal;
t_mpu9150_cal accel_cal_data;

int use_mag_cal;
t_mpu9150_cal mag_cal_data;

void mpu9150_set_debug(int on)
{
	debug_on = on;
}

int mpu9150_init(int i2c_bus, int sample_rate, int mix_factor, int rotation)
{
											
	signed char gyro_orientation[9];
																
	set_orientation(rotation, gyro_orientation);		
	
	if (i2c_bus < 0 || i2c_bus > 3)
		return -1;

	if (sample_rate < 2 || sample_rate > 50)
		return -1;

	if (mix_factor < 0 || mix_factor > 100)
		return -1;

	yaw_mixing_factor = mix_factor;

	linux_set_i2c_bus(i2c_bus);

	printf("\nInitializing IMU .");
	fflush(stdout);

	if (mpu_init(NULL)) {
		printf("\nmpu_init() failed\n");
		return -1;
	}

	printf(".");
	fflush(stdout);

	if (mpu_set_sensors(INV_XYZ_GYRO | INV_XYZ_ACCEL | INV_XYZ_COMPASS)) {
		printf("\nmpu_set_sensors() failed\n");
		return -1;
	}

	printf(".");
	fflush(stdout);

	if (mpu_configure_fifo(INV_XYZ_GYRO | INV_XYZ_ACCEL)) {
		printf("\nmpu_configure_fifo() failed\n");
		return -1;
	}

	printf(".");
	fflush(stdout);
	
	if (mpu_set_sample_rate(sample_rate)) {
		printf("\nmpu_set_sample_rate() failed\n");
		return -1;
	}

	printf(".");
	fflush(stdout);

	if (mpu_set_compass_sample_rate(sample_rate)) {
		printf("\nmpu_set_compass_sample_rate() failed\n");
		return -1;
	}

	printf(".");
	fflush(stdout);

	if (dmp_load_motion_driver_firmware()) {
		printf("\ndmp_load_motion_driver_firmware() failed\n");
		return -1;
	}

	printf(".");
	fflush(stdout);

	if (dmp_set_orientation(inv_orientation_matrix_to_scalar(gyro_orientation))) {
		printf("\ndmp_set_orientation() failed\n");
		return -1;
	}

	printf(".");
	fflush(stdout);

  	if (dmp_enable_feature(DMP_FEATURE_6X_LP_QUAT | DMP_FEATURE_SEND_RAW_ACCEL 
						| DMP_FEATURE_SEND_CAL_GYRO | DMP_FEATURE_GYRO_CAL)) {
		printf("\ndmp_enable_feature() failed\n");
		return -1;
	}

	printf(".");
	fflush(stdout);
 
	if (dmp_set_fifo_rate(sample_rate)) {
		printf("\ndmp_set_fifo_rate() failed\n");
		return -1;
	}

	printf(".");
	fflush(stdout);

	if (mpu_set_dmp_state(1)) {
		printf("\nmpu_set_dmp_state(1) failed\n");
		return -1;
	}

	printf(" done\n\n");

	return 0;
}

void mpu9150_exit()
{
	// turn off the DMP on exit 
	if (mpu_set_dmp_state(0))
		printf("mpu_set_dmp_state(0) failed\n");

	// TODO: Should turn off the sensors too
}

void mpu9150_set_accel_cal(t_mpu9150_cal *cal)
{
	int i;
	long bias[3];

	if (!cal) {
		use_accel_cal = 0;
		return;
	}

	memcpy(&accel_cal_data, cal, sizeof(t_mpu9150_cal));

	for (i = 0; i < 3; i++) {
		if (accel_cal_data.range[i] < 1)
			accel_cal_data.range[i] = 1;
		else if (accel_cal_data.range[i] > ACCEL_SENSOR_RANGE)
			accel_cal_data.range[i] = ACCEL_SENSOR_RANGE;

		bias[i] = -accel_cal_data.offset[i];
	}

	if (debug_on) {
		printf("\naccel cal (range : offset)\n");

		for (i = 0; i < 3; i++)
			printf("%d : %d\n", accel_cal_data.range[i], accel_cal_data.offset[i]);
	}

	mpu_set_accel_bias(bias);

	use_accel_cal = 1;
}

void mpu9150_set_mag_cal(t_mpu9150_cal *cal)
{
	int i;

	if (!cal) {
		use_mag_cal = 0;
		return;
	}

	memcpy(&mag_cal_data, cal, sizeof(t_mpu9150_cal));

	for (i = 0; i < 3; i++) {
		if (mag_cal_data.range[i] < 1)
			mag_cal_data.range[i] = 1;
		else if (mag_cal_data.range[i] > MAG_SENSOR_RANGE)
			mag_cal_data.range[i] = MAG_SENSOR_RANGE;

		if (mag_cal_data.offset[i] < -MAG_SENSOR_RANGE)
			mag_cal_data.offset[i] = -MAG_SENSOR_RANGE;
		else if (mag_cal_data.offset[i] > MAG_SENSOR_RANGE)
			mag_cal_data.offset[i] = MAG_SENSOR_RANGE;
	}

	if (debug_on) {
		printf("\nmag cal (range : offset)\n");

		for (i = 0; i < 3; i++)
			printf("%d : %d\n", mag_cal_data.range[i], mag_cal_data.offset[i]);
	}

	use_mag_cal = 1;
}

int mpu9150_read_dmp(mpudata_t *mpu)
{
	short sensors;
	unsigned char more;

	if (!data_ready())
		return -1;

	if (dmp_read_fifo(mpu->rawGyro, mpu->rawAccel, mpu->rawQuat, &mpu->dmpTimestamp, &sensors, &more) < 0) {
		if(g_debug > 1)printf("dmp_read_fifo() failed\n");
		return -1;
	}

	while (more) {
		// Fell behind, reading again
		if (dmp_read_fifo(mpu->rawGyro, mpu->rawAccel, mpu->rawQuat, &mpu->dmpTimestamp, &sensors, &more) < 0) {
			if(g_debug > 1)printf("dmp_read_fifo() failed [2]\n");
			return -1;
		}
	}

	return 0;
}

int mpu9150_read_mag(mpudata_t *mpu)
{
	if (mpu_get_compass_reg(mpu->rawMag, &mpu->magTimestamp) < 0) {
		printf("mpu_get_compass_reg() failed\n");
		return -1;
	}

	return 0;
}

int mpu9150_read(mpudata_t *mpu)
{
	if (mpu9150_read_dmp(mpu) != 0)
		return -1;

	if (mpu9150_read_mag(mpu) != 0)
		return -1;

	calibrate_data(mpu);

	return data_fusion(mpu);
}

int data_ready()
{
	short status;

	if (mpu_get_int_status(&status) < 0) {
		printf("mpu_get_int_status() failed\n");
		return 0;
	}

	// debug
	//if (status != 0x0103)
	//	fprintf(stderr, "%04X\n", status);

	return (status == 0x0103);
}

void calibrate_data(mpudata_t *mpu)
{
	if (use_mag_cal) {
      mpu->calibratedMag[VEC3_Y] = -(short)(((long)(mpu->rawMag[VEC3_X] - mag_cal_data.offset[VEC3_X])
			* (long)MAG_SENSOR_RANGE) / (long)mag_cal_data.range[VEC3_X]);

      mpu->calibratedMag[VEC3_X] = (short)(((long)(mpu->rawMag[VEC3_Y] - mag_cal_data.offset[VEC3_Y])
			* (long)MAG_SENSOR_RANGE) / (long)mag_cal_data.range[VEC3_Y]);

      mpu->calibratedMag[VEC3_Z] = (short)(((long)(mpu->rawMag[VEC3_Z] - mag_cal_data.offset[VEC3_Z])
			* (long)MAG_SENSOR_RANGE) / (long)mag_cal_data.range[VEC3_Z]);
	}
	else {
		mpu->calibratedMag[VEC3_Y] = -mpu->rawMag[VEC3_X];
		mpu->calibratedMag[VEC3_X] = mpu->rawMag[VEC3_Y];
		mpu->calibratedMag[VEC3_Z] = mpu->rawMag[VEC3_Z];
	}

	if (use_accel_cal) {
      mpu->calibratedAccel[VEC3_X] = -(short)(((long)mpu->rawAccel[VEC3_X] * (long)ACCEL_SENSOR_RANGE)
			/ (long)accel_cal_data.range[VEC3_X]);

      mpu->calibratedAccel[VEC3_Y] = (short)(((long)mpu->rawAccel[VEC3_Y] * (long)ACCEL_SENSOR_RANGE)
			/ (long)accel_cal_data.range[VEC3_Y]);

      mpu->calibratedAccel[VEC3_Z] = (short)(((long)mpu->rawAccel[VEC3_Z] * (long)ACCEL_SENSOR_RANGE)
			/ (long)accel_cal_data.range[VEC3_Z]);
	}
	else {
		mpu->calibratedAccel[VEC3_X] = -mpu->rawAccel[VEC3_X];
		mpu->calibratedAccel[VEC3_Y] = mpu->rawAccel[VEC3_Y];
		mpu->calibratedAccel[VEC3_Z] = mpu->rawAccel[VEC3_Z];
	}
}

void tilt_compensate(quaternion_t magQ, quaternion_t unfusedQ)
{
	quaternion_t unfusedConjugateQ;
	quaternion_t tempQ;

	quaternionConjugate(unfusedQ, unfusedConjugateQ);
	quaternionMultiply(magQ, unfusedConjugateQ, tempQ);
	quaternionMultiply(unfusedQ, tempQ, magQ);
}

int data_fusion(mpudata_t *mpu)
{
	quaternion_t dmpQuat;
	vector3d_t dmpEuler;
	quaternion_t magQuat;
	quaternion_t unfusedQuat;
	float deltaDMPYaw;
	float deltaMagYaw;
	float newMagYaw;
	float newYaw;
	
	dmpQuat[QUAT_W] = (float)mpu->rawQuat[QUAT_W];
	dmpQuat[QUAT_X] = (float)mpu->rawQuat[QUAT_X];
	dmpQuat[QUAT_Y] = (float)mpu->rawQuat[QUAT_Y];
	dmpQuat[QUAT_Z] = (float)mpu->rawQuat[QUAT_Z];

	quaternionNormalize(dmpQuat);	
	quaternionToEuler(dmpQuat, dmpEuler);

	mpu->fusedEuler[VEC3_X] = dmpEuler[VEC3_X];
	mpu->fusedEuler[VEC3_Y] = -dmpEuler[VEC3_Y];
	mpu->fusedEuler[VEC3_Z] = 0;

	eulerToQuaternion(mpu->fusedEuler, unfusedQuat);

	deltaDMPYaw = -dmpEuler[VEC3_Z] + mpu->lastDMPYaw;
	mpu->lastDMPYaw = dmpEuler[VEC3_Z];

	magQuat[QUAT_W] = 0;
	magQuat[QUAT_X] = mpu->calibratedMag[VEC3_X];
  	magQuat[QUAT_Y] = mpu->calibratedMag[VEC3_Y];
  	magQuat[QUAT_Z] = mpu->calibratedMag[VEC3_Z];

	tilt_compensate(magQuat, unfusedQuat);

	newMagYaw = -atan2f(magQuat[QUAT_Y], magQuat[QUAT_X]);

	if (newMagYaw != newMagYaw) {
		printf("newMagYaw NAN\n");
		return -1;
	}

	if (newMagYaw < 0.0f)
		newMagYaw = TWO_PI + newMagYaw;

	newYaw = mpu->lastYaw + deltaDMPYaw;

	if (newYaw > TWO_PI)
		newYaw -= TWO_PI;
	else if (newYaw < 0.0f)
		newYaw += TWO_PI;
	 
	deltaMagYaw = newMagYaw - newYaw;
	
	if (deltaMagYaw >= (float)M_PI)
		deltaMagYaw -= TWO_PI;
	else if (deltaMagYaw < -(float)M_PI)
		deltaMagYaw += TWO_PI;

	if (yaw_mixing_factor > 0)
		newYaw += deltaMagYaw / yaw_mixing_factor;

	if (newYaw > TWO_PI)
		newYaw -= TWO_PI;
	else if (newYaw < 0.0f)
		newYaw += TWO_PI;

	mpu->lastYaw = newYaw;

	if (newYaw > (float)M_PI)
		newYaw -= TWO_PI;

	mpu->fusedEuler[VEC3_Z] = newYaw;

	eulerToQuaternion(mpu->fusedEuler, mpu->fusedQuat);

	return 0;
}

/* These next two functions convert the orientation matrix (see
 * gyro_orientation) to a scalar representation for use by the DMP.
 * NOTE: These functions are borrowed from InvenSense's MPL.
 */
unsigned short inv_row_2_scale(const signed char *row)
{
    unsigned short b;

    if (row[0] > 0)
        b = 0;
    else if (row[0] < 0)
        b = 4;
    else if (row[1] > 0)
        b = 1;
    else if (row[1] < 0)
        b = 5;
    else if (row[2] > 0)
        b = 2;
    else if (row[2] < 0)
        b = 6;
    else
        b = 7;      // error
    return b;
}

unsigned short inv_orientation_matrix_to_scalar(signed char *mtx)
{
    unsigned short scalar;
    /*
       XYZ  010_001_000 Identity Matrix
       XZY  001_010_000
       YXZ  010_000_001
       YZX  000_010_001
       ZXY  001_000_010
       ZYX  000_001_010
     */
    scalar = inv_row_2_scale(mtx);
    scalar |= inv_row_2_scale(mtx + 3) << 3;
    scalar |= inv_row_2_scale(mtx + 6) << 6;
    return scalar;
}




/**
 * MPU is orientated with pin1 away from pressure inlets 
 * (i.e. away from dir of travel on x axis)
 * y axis is towards cinch socket, which is towards right wing in "normal" orientation
 * z axis is upside down in "normal" orientation, 
 * 
 * -------
 * |     x|
 * |      |
 * |______|
 * 
 * 
 * Gyro
 * -----
 * In above orientation:
 * X-axis = BACKWD
 * Y-axis = Right
 * z-axis = UP
 * 
 * ==> in installed orientation, normal landscape:
 * X = FWD
 * Y = RH Wing
 * Z = DOWN
			{ 1, 0, 0,
			  0, 1, 0, 
			  0, 0,-1 };
 * 
 * ==> portrait clockwise:
 * X = FWD
 * Y = DOWN
 * Z = LH Wing
			{ 1, 0, 0,
			  0, 0,-1, 
			  0,-1, 0 };	  
 * 
 * ==> landscape upside down:
 * X = FWD
 * Y = LH Wing
 * Z = UP
			{ 1, 0, 0,
			  0,-1, 0, 
			  0, 0, 1 };	  
 *
 * ==> Portrait anticlockwise:
 * X = FWD
 * Y = UP
 * Z = RH Wing
			{ 1, 0, 0,
			  0, 0, 1, 
			  0, 1, 0 };
 * 
 * Magnetometer
 * -------------
 * 
 * In above orientation:
 * X = Right
 * Y = Backward
 * Z = Down
 * 
 * ==> in installed orientation, landscape:
 * X = Right Wing
 * Y = Fwd
 * Z = Up
 * 
 * ==> Portrait clockwise
 * X = Down
 * Y = Fwd
 * Z = Right Wing
 * 
 * ==> Upside down landscape:
 * X = Left Wing
 * Y = Fwd
 * Z = Down
 * 
 * ==> Portrait anticlockwise:
 * X = Up
 * Y = Fwd
 * Z = Left Wing
 * 
 */



int set_orientation(int rotation, signed char gyro_orientation[9])
{
	
	// normal landscape
	signed char gyro_orientation_0[9]	= { 1, 0, 0,
						    0,-1, 0,
						    0, 0, -1 };
	// portrait 90deg	
	signed char gyro_orientation_1[9]	= { 1, 0, 0,
						    0, 0, 1, 
						    0, -1, 0 };
	// landscape 180deg	
	signed char gyro_orientation_2[9]	= { 1, 0, 0,
						    0, 1, 0, 
						    0, 0, 1 };
	// portrait 270deg
	signed char gyro_orientation_3[9]	= { 1, 0, 0,
						    0, 0, -1, 
						    0, 1, 0 };	
/*											
	//signed char gyro_orientation[9];
	int orientation, charval;


	FILE *f;
	char *sys_conf_file = SYS_CONF_FILE;
	char buff[10], rot[10]; 
	short int found_orientation = 0;
	
	f = fopen(sys_conf_file, "r");
	
	if(f)
	{
		while(!found_orientation)
		{
			if (!fgets(buff, 10, f)) 
			{
				printf("Unable to find valid rotation directive in %s", sys_conf_file);
				break;
			}
		
			if((strstr(buff, "rotation=") != NULL) || (strstr(buff, "rotation ") != NULL))
			{	
				while(!found_orientation)
				{
					if (!fgets(rot, 2, f)) 
						break;
					
					charval = atoi(rot);
					if(((charval > 0) && (charval < 4))|| (strcmp(rot, "0") == 0)) 
					{
						found_orientation = 1;
						orientation =  charval;
						break;
					}
				}
				
			}
		} 
		
		fclose(f);
	} 
	else
		printf("Cannot determine orientation from system config file!");
	*/
	
	printf("Using orientation %d", rotation);
	
	switch(rotation) 
	{
		case 1: 	
			memcpy(gyro_orientation, gyro_orientation_1, sizeof gyro_orientation_1);
			break;																		
		case 2: 	
			memcpy(gyro_orientation, gyro_orientation_2, sizeof gyro_orientation_2);
			break;																		
		case 3: 	
			memcpy(gyro_orientation, gyro_orientation_3, sizeof gyro_orientation_3);	
			break;																	
		case 0: 	
		default:
			rotation = 0;
			memcpy(gyro_orientation, gyro_orientation_0, sizeof gyro_orientation_0);	
	}																	

	return rotation;
	
}
