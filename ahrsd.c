/**
 * 
 * OpenVario MPU-9150 AHRS Driver
 * (c) John Wells
 * (c) Pansenti
 * 
 * Adapted from original code by Pansenti, Inc. See accompanying LICENSE file for details.
 * 
 * Reads the Invensense MPU9150 IMU and pushes fused euler angles to 
 * TCP socket for XCSoar in a format compatiblewith the LevilAHRS driver..
 * 
 * Currently no attempt made to smooth output beyond what is done in the
 * Invensense driver, or to transform the result for different Oenvario orientations.
 * 
 * TODO:
 * Daemonise
 * Add default orientation to calibration
 * Update default sampling rate
 * Integrate with OV sensord / sensorcal
 * 
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "mpu9150.h"
#include "linux_glue.h"
#include "local_defaults.h"

int set_cal(int mag, char *cal_file);
void read_loop(unsigned int sample_rate);
void print_rpyl(mpudata_t *mpu, int sock);
void print_fused_euler_angles(mpudata_t *mpu);
void print_fused_quaternion(mpudata_t *mpu);
void print_calibrated_accel(mpudata_t *mpu);
void print_calibrated_mag(mpudata_t *mpu);
void register_sig_handler();
void sigint_handler(int sig);

int done;

void usage(char *argv_0)
{
	printf("\nUsage: %s [options]\n", argv_0);
	printf("  -b <i2c-bus>          The I2C bus number where the IMU is. The default is 1 to use /dev/i2c-1.\n");
	printf("  -s <sample-rate>      The IMU sample rate in Hz. Range 2-50, default 10.\n");
	printf("  -y <yaw-mix-factor>   Effect of mag yaw on fused yaw data.\n");
	printf("                           0 = gyro only\n");
	printf("                           1 = mag only\n");
	printf("                           > 1 scaled mag adjustment of gyro data\n");
	printf("                           The default is 4.\n");
	printf("  -a <accelcal file>    Path to accelerometer calibration file. Default is ./accelcal.txt\n");
	printf("  -m <magcal file>      Path to mag calibration file. Default is ./magcal.txt\n");
	printf("  -v                    Verbose messages\n");
	printf("  -h                    Show this help\n");

	printf("\nExample: %s -b3 -s20 -y10\n\n", argv_0);
	
	exit(1);
}

int main(int argc, char **argv)
{
	int opt, len;
	int i2c_bus = DEFAULT_I2C_BUS;
	int sample_rate = DEFAULT_SAMPLE_RATE_HZ;
	int yaw_mix_factor = DEFAULT_YAW_MIX_FACTOR;
	int verbose = 0;
	char *mag_cal_file = NULL;
	char *accel_cal_file = NULL;
	
	while ((opt = getopt(argc, argv, "b:s:y:a:m:vh")) != -1) {
		switch (opt) {
		case 'b':
			i2c_bus = strtoul(optarg, NULL, 0);
			
			if (errno == EINVAL)
				usage(argv[0]);
			
			if (i2c_bus < 0 || i2c_bus > 5)
				usage(argv[0]);

			break;
		
		case 's':
			sample_rate = strtoul(optarg, NULL, 0);
			
			if (errno == EINVAL)
				usage(argv[0]);
			
			if (sample_rate < 2 || sample_rate > 50)
				usage(argv[0]);

			break;

		case 'y':
			yaw_mix_factor = strtoul(optarg, NULL, 0);
			
			if (errno == EINVAL)
				usage(argv[0]);
			
			if (yaw_mix_factor < 0 || yaw_mix_factor > 100)
				usage(argv[0]);

			break;

		case 'a':
			len = 1 + strlen(optarg);

			accel_cal_file = (char *)malloc(len);

			if (!accel_cal_file) {
				perror("malloc");
				exit(1);
			}

			strcpy(accel_cal_file, optarg);
			break;

		case 'm':
			len = 1 + strlen(optarg);

			mag_cal_file = (char *)malloc(len);

			if (!mag_cal_file) {
				perror("malloc");
				exit(1);
			}

			strcpy(mag_cal_file, optarg);
			break;

		case 'v':
			verbose = 1;
			break;

		case 'h':
		default:
			usage(argv[0]);
			break;
		}
	}

	register_sig_handler();

	mpu9150_set_debug(verbose);

	if (mpu9150_init(i2c_bus, sample_rate, yaw_mix_factor))
		exit(1);

	set_cal(0, accel_cal_file);
	set_cal(1, mag_cal_file);

	if (accel_cal_file)
		free(accel_cal_file);

	if (mag_cal_file)
		free(mag_cal_file);

	read_loop(sample_rate);

	mpu9150_exit();

	return 0;
}

void read_loop(unsigned int sample_rate)
{
	unsigned long loop_delay;
	mpudata_t mpu;

	int sock_imu_err = 0;
	int sock_imu =  0;
	struct sockaddr_in server_imu;

	memset(&mpu, 0, sizeof(mpudata_t));

	if (sample_rate == 0)
		return;


	server_imu.sin_addr.s_addr = inet_addr("127.0.0.1");
	server_imu.sin_family = AF_INET;
	server_imu.sin_port = htons(4354);


	// try to connect to XCSoar
	while (connect(sock_imu, (struct sockaddr *)&server_imu, sizeof(server_imu)) < 0) {
		fprintf(stderr, "failed to connect, trying again\n");
		fflush(stdout);
		sleep(1);
	}


	loop_delay = (1000 / sample_rate) - 2;

	printf("\nEntering read loop (ctrl-c to exit)\n\n");

	linux_delay_ms(loop_delay);

	while (!done && !sock_imu_err) {
		if (mpu9150_read(&mpu) == 0) {
			print_rpyl(&mpu, sock_imu);
		}

		linux_delay_ms(loop_delay);
	}

	printf("\n\n");
}



/**
* output_rpyl: Output in NMEA format as
* expected by XCSoar LevilAHRS Driver:
* $RPYL,Roll,Pitch,MagnHeading,SideSlip,YawRate,G,errorcode
*
* Error bits (not implemented yet):
*   0: Roll gyro test failed  
*   1: Roll gyro test failed 
*   2: Roll gyro test failed 
*   3: Acc X test failed 
*   4: Acc Y test failed 
*   5: Acc Z test failed 
*   6: Watchdog test failed
*   7: Ram test failed
*   8: EEPROM access test failed
*   9: EEPROM checksum test failed
*  10: Flash checksum test failed
*  11: Low voltage error
*  12: High temperature error (>60 C)
*  13: Inconsistent roll data between gyro and acc.
*  14: Inconsistent pitch data between gyro and acc.
*  15: Inconsistent yaw data between gyro and acc.
*/
void print_rpyl(mpudata_t *mpu, int sock)
{
	
	int sock_err;
	char s[256];
	
	sprintf(s, "\r$RPYL,%0.0f,%0.0f,%0.0f,0,0,0,0	",
	       		mpu->fusedEuler[VEC3_X] * RAD_TO_DEGREE,
	       		- (mpu->fusedEuler[VEC3_Y] * RAD_TO_DEGREE),
	       		mpu->fusedEuler[VEC3_Z] * RAD_TO_DEGREE);
	
	// Send NMEA string via socket to XCSoar
	if ((sock_err = send(sock, s, strlen(s), 0)) < 0)
	{	
		//fprintf(stderr, "send failed\n");
		printf("send failed\n");
		//break;
	}
	
	
}







void print_fused_euler_angles(mpudata_t *mpu)
{
	printf("\rX: %0.0f Y: %0.0f Z: %0.0f        ",
			mpu->fusedEuler[VEC3_X] * RAD_TO_DEGREE, 
			mpu->fusedEuler[VEC3_Y] * RAD_TO_DEGREE, 
			mpu->fusedEuler[VEC3_Z] * RAD_TO_DEGREE);

	fflush(stdout);
}


void print_fused_quaternions(mpudata_t *mpu)
{
	printf("\rW: %0.2f X: %0.2f Y: %0.2f Z: %0.2f        ",
			mpu->fusedQuat[QUAT_W],
			mpu->fusedQuat[QUAT_X],
			mpu->fusedQuat[QUAT_Y],
			mpu->fusedQuat[QUAT_Z]);

	fflush(stdout);
}

void print_calibrated_accel(mpudata_t *mpu)
{
	printf("\rX: %05d Y: %05d Z: %05d        ",
			mpu->calibratedAccel[VEC3_X], 
			mpu->calibratedAccel[VEC3_Y], 
			mpu->calibratedAccel[VEC3_Z]);

	fflush(stdout);
}

void print_calibrated_mag(mpudata_t *mpu)
{
	printf("\rX: %03d Y: %03d Z: %03d        ",
			mpu->calibratedMag[VEC3_X], 
			mpu->calibratedMag[VEC3_Y], 
			mpu->calibratedMag[VEC3_Z]);

	fflush(stdout);
}

int set_cal(int mag, char *cal_file)
{
	int i;
	FILE *f;
	char buff[32];
	long val[6];
	caldata_t cal;

	if (cal_file) {
		f = fopen(cal_file, "r");
		
		if (!f) {
			perror("open(<cal-file>)");
			return -1;
		}
	}
	else {
		if (mag) {
			f = fopen("./magcal.txt", "r");
		
			if (!f) {
				printf("Default magcal.txt not found\n");
				return 0;
			}
		}
		else {
			f = fopen("./accelcal.txt", "r");
		
			if (!f) {
				printf("Default accelcal.txt not found\n");
				return 0;
			}
		}		
	}

	memset(buff, 0, sizeof(buff));
	
	for (i = 0; i < 6; i++) {
		if (!fgets(buff, 20, f)) {
			printf("Not enough lines in calibration file\n");
			break;
		}

		val[i] = atoi(buff);

		if (val[i] == 0) {
			printf("Invalid cal value: %s\n", buff);
			break;
		}
	}

	fclose(f);

	if (i != 6) 
		return -1;

	cal.offset[0] = (short)((val[0] + val[1]) / 2);
	cal.offset[1] = (short)((val[2] + val[3]) / 2);
	cal.offset[2] = (short)((val[4] + val[5]) / 2);

	cal.range[0] = (short)(val[1] - cal.offset[0]);
	cal.range[1] = (short)(val[3] - cal.offset[1]);
	cal.range[2] = (short)(val[5] - cal.offset[2]);
	
	if (mag) 
		mpu9150_set_mag_cal(&cal);
	else 
		mpu9150_set_accel_cal(&cal);

	return 0;
}

void register_sig_handler()
{
	struct sigaction sia;

	bzero(&sia, sizeof sia);
	sia.sa_handler = sigint_handler;

	if (sigaction(SIGINT, &sia, NULL) < 0) {
		perror("sigaction(SIGINT)");
		exit(1);
	} 
}

void sigint_handler(int sig)
{
	done = 1;
}