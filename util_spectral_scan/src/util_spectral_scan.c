/*
  ______                              _
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2014 Semtech-Cycleo

Description:
    SX1301 spectral scan

License: Revised BSD License, see LICENSE.TXT file include in the project
Maintainer: Matthieu Leurent
*/


/* -------------------------------------------------------------------------- */
/* --- DEPENDENCIES --------------------------------------------------------- */

/* Fix an issue between POSIX and C99 */
#if __STDC_VERSION__ >= 199901L
    #define _XOPEN_SOURCE 600
#else
    #define _XOPEN_SOURCE 500
#endif

#include <stdint.h>     /* C99 types */
#include <stdio.h>      /* NULL printf */
#include <stdlib.h>     /* EXIT atoi */
#include <unistd.h>     /* getopt */
#include <string.h>

#include "loragw_fpga_spi.h"
#include "loragw_fpga_reg.h"
#include "loragw_fpga_aux.h"

/* -------------------------------------------------------------------------- */
/* --- MACROS & CONSTANTS --------------------------------------------------- */

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define DEFAULT_START_FREQ   863000000   /* start frequency, Hz */
#define DEFAULT_STOP_FREQ    870000000   /* stop frequency, Hz */
#define DEFAULT_STEP_FREQ       200000   /* frequency step, Hz */
#define DEFAULT_RSSI_PTS         65535   /* number of RSSI reads */
#define DEFAULT_RSSI_RATE_DIV        1   /* RSSI sampling rate = 32MHz/(div+1030)*/
#define DEFAULT_LOG_NAME   "rssi_histogram"

#define RSSI_RANGE   256
#define RSSI_OFFSET  -135

#define MAX_FREQ   1000000000
#define MIN_FREQ    800000000
#define MIN_STEP_FREQ    5000

/* -------------------------------------------------------------------------- */
/* --- GLOBAL VARIABLES ----------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/* --- SUBFUNCTIONS DECLARATION --------------------------------------------- */

int setup_sx1272( uint32_t freq );

/* -------------------------------------------------------------------------- */
/* --- MAIN FUNCTION -------------------------------------------------------- */

int main( int argc, char ** argv )
{
    int i, j, k; /* loop and temporary variables */
    int x; /* return code for functions */
    int32_t reg_val;
    
    /* Parameter parsing */
    double arg_lf[3] = {0,0,0};
    unsigned arg_u = 0;
    char arg_s[64];
    
    /* Application parameters */
    uint32_t start_freq = DEFAULT_START_FREQ;
    uint32_t stop_freq = DEFAULT_STOP_FREQ;
    uint32_t step_freq = DEFAULT_STEP_FREQ;
    uint16_t rssi_pts = DEFAULT_RSSI_PTS;
    uint16_t rssi_rate_div = DEFAULT_RSSI_RATE_DIV;
    char log_file_name[64] = DEFAULT_LOG_NAME;
    FILE * log_file = NULL;
    
    /* Local var */
    int freq_nb;
    uint32_t freq;
    uint8_t read_burst[RSSI_RANGE*2];
    uint16_t rssi_histo;
    uint16_t rssi_cumu;
    float rssi_thresh[] = {0.1,0.3,0.5,0.8,1};
    
    /* FPGA settings */
    uint32_t input_sync_edge  = 0;
    uint32_t output_sync_edge = 0;
    uint32_t filt_on = 1;
    
    /* Parse command line options */
    while( (i = getopt( argc, argv, "hud::f:n:r:l:" )) != -1 )
    {
        switch( i )
        {
        case 'h':
            printf( "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n" );
            printf( " -f <float>:<float>:<float>  Frequency vector to scan in MHz (start:step:stop)\n" );
            printf( "                               start>%3.3f step>%1.3f stop<%3.3f\n", MIN_FREQ/1e6, MIN_STEP_FREQ/1e6, MAX_FREQ/1e6 );
            printf( " -n <uint>  Total number of RSSI points, [1,65535]\n" );
            printf( " -r <uint>  Divide factor of RSSI sampling rate, 32MHz/(div+1030), [1,65535]\n" );
            printf( " -l <char>  Log file name\n" );
            printf( "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n" );
            return EXIT_SUCCESS;
            break;
        
        case 'f': /* -f <float>:<float>:<float>  Frequency vector to scan in MHz, start:step:stop */
            j = sscanf( optarg, "%lf:%lf:%lf", &arg_lf[0], &arg_lf[1], &arg_lf[2] );
            if( (j!=3) || (arg_lf[0] < MIN_FREQ/1e6) || (arg_lf[0] > MAX_FREQ/1e6) || (arg_lf[1] < MIN_STEP_FREQ/1e6) || (arg_lf[2] < MIN_FREQ/1e6) || (arg_lf[2] > MAX_FREQ/1e6) )
            {
                printf( "ERROR: argument parsing of -f argument. -h for help.\n" );
                return EXIT_FAILURE;
            }
            else
            {
                start_freq = (uint32_t)((arg_lf[0] * 1e6) + 0.5); /* .5 Hz offset to get rounding instead of truncating */
                step_freq = (uint32_t)((arg_lf[1] * 1e6) + 0.5); /* .5 Hz offset to get rounding instead of truncating */
                stop_freq = (uint32_t)((arg_lf[2] * 1e6) + 0.5); /* .5 Hz offset to get rounding instead of truncating */
            }
            break;
        
        case 'n': /* -n <uint>  Total number of RSSI points, [1,65535] */
            j = sscanf( optarg, "%i", &arg_u );
            if( (j != 1) || (arg_u < 1) || (arg_u > 65535) )
            {
                printf( "ERROR: argument parsing of -n argument. -h for help.\n" );
                return EXIT_FAILURE;
            }
            else
            {
                rssi_pts = (uint16_t)arg_u;
            }
            break;
        
        case 'r': /* -r <uint>  Divide factor of RSSI sampling rate, 32MHz/(div+1030), [1,65535] */
            j = sscanf( optarg, "%i", &arg_u );
            if( (j != 1) || (arg_u < 1) || (arg_u > 65535) )
            {
                printf( "ERROR: argument parsing of -r argument. -h for help.\n" );
                return EXIT_FAILURE;
            }
            else
            {
                rssi_rate_div = (uint16_t)arg_u;
            }
            break;
        
        case 'l': /* -l <char>  Log file name */
            j = sscanf( optarg, "%s", arg_s );
            if( j != 1 )
            {
                printf( "ERROR: argument parsing of -l argument. -h for help.\n" );
                return EXIT_FAILURE;
            }
            else
            {
                sprintf(log_file_name, "%s", arg_s);
            }
            break;
        
        default:
            printf( "ERROR: argument parsing options. -h for help.\n" );
            return EXIT_FAILURE;
        }
    }
    
    /* Start message */
    printf( "+++ Start spectral scan of LoRa gateway channels +++\n" );
    
    x = lgw_fpga_connect( );
    if( x != 0 )
    {
        printf( "ERROR: Failed to connect to FPGA\n" );
        return EXIT_FAILURE;
    }
    
    /* Configure FPGA */
    x = lgw_fpga_reg_w(LGW_FPGA_SOFT_RESET, 1);
    x |= lgw_fpga_reg_w(LGW_FPGA_SOFT_RESET, 0);
    x |= lgw_fpga_reg_w(LGW_FPGA_FPGA_CTRL, (filt_on << 4) | (input_sync_edge << 2)| (output_sync_edge << 3) | (1 << 1)); /* Reset Radio */
    x |= lgw_fpga_reg_w(LGW_FPGA_FPGA_CTRL, (filt_on << 4) | (input_sync_edge << 2)| (output_sync_edge << 3));
    x |= lgw_fpga_reg_w(LGW_FPGA_HISTO_TEMPO, rssi_rate_div);
    x |= lgw_fpga_reg_w(LGW_FPGA_HISTO_NB_READ, rssi_pts);
    if( x != LGW_REG_SUCCESS )
    {
        printf( "ERROR: Failed to configure FPGA\n" );
        return EXIT_FAILURE;
    }
    
    /* create log file */
    strcat(log_file_name,".csv");
    log_file = fopen(log_file_name, "w");
    if (log_file == NULL)
    {
        printf( "ERROR: impossible to create log file %s\n", log_file_name );
        return EXIT_FAILURE;
    }
    printf( "Writing to file: %s\n", log_file_name );
    
    /* Number of frequency steps */
    freq_nb = (int)( (stop_freq - start_freq) / step_freq ) + 1;
    printf( "Scanning frequencies:\nstart: %d Hz\nstop : %d Hz\nstep : %d Hz\nnb   : %d\n", start_freq, stop_freq, step_freq, freq_nb );
    
    /* Main loop */
    for( j = 0; j < freq_nb; j++ )
    {
        /* Current frequency */
        freq = start_freq + j * step_freq;
        printf( "%d", freq );
        
        /* Set SX1272 */
        x |= setup_sx1272( freq );
        if( x != 0 )
        {
            printf( "ERROR: SX1272 setup failed\n" );
            return EXIT_FAILURE;
        }
        
        /* Start histogram */
        lgw_fpga_reg_w(LGW_FPGA_FPGA_CTRL, 1);
        
        /* Wait until rssi_pts have been processed */
        do
        {
            wait_ms(1000);
            lgw_fpga_reg_r(LGW_FPGA_FPGA_STATUS, &reg_val);
        }
        while( (reg_val & 0x0F) != 8 );
        
        /* Stop histogram */
        lgw_fpga_reg_w(LGW_FPGA_FPGA_CTRL, 0);
        
        /* Read histogram */
        lgw_fpga_reg_w(LGW_FPGA_HISTO_RAM_ADDR, 0);
        lgw_fpga_reg_rb(LGW_FPGA_HISTO_RAM_DATA, read_burst, RSSI_RANGE*2);
        
        fprintf( log_file, "%d", freq );
        rssi_cumu = 0;
        k = 0;
        for( i = 0; i < RSSI_RANGE; i++ )
        {
            rssi_histo = (uint16_t)read_burst[2*i] | ((uint16_t)read_burst[2*i+1] << 8);
            fprintf(log_file, ",%d,%d", i+RSSI_OFFSET, rssi_histo);
            rssi_cumu += rssi_histo;
            if( rssi_cumu > rssi_thresh[k]*rssi_pts )
            {
                printf( "  %d%%<%4d", (uint16_t)(rssi_thresh[k]*100), i+RSSI_OFFSET );
                k++;
            }
        }
        fprintf( log_file, "\n" );
        printf( "\n" );
    }
    fclose( log_file );
    
    /* FGPA soft reset */
    x = lgw_fpga_reg_w(LGW_FPGA_SOFT_RESET, 1); 
    x |= lgw_fpga_reg_w(LGW_FPGA_SOFT_RESET, 0);
    
    /* Close SPI */
    x |= lgw_fpga_disconnect( );
    
    if( x != 0 )
    {
        printf( "ERROR: Failed to disconnect FPGA\n" );
        return EXIT_FAILURE;
    }
    
    printf( "INFO: Successfully disconnected FPGA\n" );
    
    printf( "+++  Exiting Spectral scan program +++\n" );
    
    return EXIT_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* --- SUBFUNCTIONS DEFINITION ---------------------------------------------- */

int setup_sx1272( uint32_t freq )
{
    uint64_t freq_reg;
    uint8_t bw = 0;
    uint8_t LowZin = 1;
    uint8_t sf = 7;
    uint8_t AgcAuto = 1;
    uint8_t LnaGain = 1;
    uint8_t TrimRxCrFo = 0;
    uint8_t LnaBoost = 3;
    uint8_t AdcBwAuto = 0;
    uint8_t AdcBw = 7;
    uint8_t AdcLowPwr = 0;
    uint8_t AdcTrim = 6;
    uint8_t AdcTest = 0;
    uint8_t reg_val;
    int x;
    
    x = lgw_sx1272_reg_r(0x42, &reg_val);
    if (x != LGW_REG_SUCCESS) {
        printf("ERROR: Failed to read SX1272 version register\n");
        return EXIT_FAILURE;
    }
    if (reg_val != 0x22) {
        printf("ERROR: Unexpected SX1272 version\n");
        return EXIT_FAILURE;
    }
    
    /* Set in LoRa mode */
    x = lgw_sx1272_reg_w(0x01, 0);
    wait_ms(100);
    x |= lgw_sx1272_reg_w(0x01, 0 | (1<<7));
    wait_ms(100);
    x |= lgw_sx1272_reg_w(0x01, 1 | (1<<7));
    wait_ms(100);

    /* Set PLL freq */
    freq_reg = ((uint64_t)freq << 19) / (uint64_t)32000000;
    x |= lgw_sx1272_reg_w(6, (freq_reg >> 16) & 0xFF);
    x |= lgw_sx1272_reg_w(7, (freq_reg >>  8) & 0xFF);
    x |= lgw_sx1272_reg_w(8,  freq_reg        & 0xFF);
    
    /* Config */
    x |= lgw_sx1272_reg_w(0x1D, bw << 6); 
    x |= lgw_sx1272_reg_w(0x50, LowZin);
    x |= lgw_sx1272_reg_w(0x1E, (sf << 4) | (AgcAuto << 2));
    x |= lgw_sx1272_reg_w(0x0C, LnaBoost | (TrimRxCrFo << 3) | (LnaGain << 5));
    x |= lgw_sx1272_reg_w(0x68, AdcBw | (AdcBwAuto << 3));
    x |= lgw_sx1272_reg_w(0x69, AdcTest | (AdcTrim << 4) | (AdcLowPwr << 7));
    
    if (x != LGW_REG_SUCCESS) {
        printf("ERROR: Failed to configure SX1272\n");
        return EXIT_FAILURE;
    }
    
    /* Set in Rx continuous mode */
    x = lgw_sx1272_reg_w(0x01, 5 | (1<<7));
    wait_ms(100);
    x |= lgw_sx1272_reg_r(0x01, &reg_val);
    if ((reg_val != (5 | (1<<7))) || (x != LGW_REG_SUCCESS)) {
        printf("ERROR: SX1272 failed to enter RX continuous mode\n");
        return EXIT_FAILURE;
    }

    //printf("INFO: Successfully configured SX1272\n");
    return EXIT_SUCCESS;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

/* --- EOF ------------------------------------------------------------------ */

