/*
 * Copyright (c) 2013 SanDisk Corporation.  All rights reserved.
 *
 * File:   interface.h
 * Author: Niranjan Neelakanta
 *
 * Created on March 25, 2013.
 */

#ifndef __FDFLICINTERFACE_H
#define __FDFLICINTERFACE_H
#ifdef  __cplusplus
extern "C" {
#endif

//This is the type of installation
enum lic_inst_type {
	LIT_INVAL,
	LIT_STAND_ALONE = 1,	//Only one machine can be installed
	LIT_MULTI_INST,		//Multiple m/cs can have same license
};

//License type w.r.t period of validity
enum lic_per_type {
	LPT_INVAL,
	LPT_EVAL,		//Evaluation copy
	LPT_DEVELOP,		//Development copy
	LPT_PERIODIC,		//Periodic renewal needed
	LPT_PERPETUAL,		//Permanent license
};

typedef	char	lic_type;

#define GET_INST_TYPE(_X) 		((_X) & 0x0f)
#define GET_PER_TYPE(_X)		(((_X) >> 4) & 0x0f)
#define MAKE_LICENSE(INST, PERIOD)	(((PERIOD) << 4) || (INST))

/*
 * Various license states. On adding new state, add corresponding error message
 * to in lic_state_msg[]
 */
enum lic_state {
	LS_VALID = 1,		//Valid license
	LS_EXPIRED,		//Expired.
	LS_NOTBEGUN,		//Invalid as license time is for future dates
	LS_INTERNAL_ERR,	//Invalid due to internal error
	LS_IO_ERR,		//Invalid due to IO error
	LS_UNSUPPORTED_FMT,	//Invalid as unsupported license file format
	LS_FORMAT_INVALID,	//Invalid as unexpected format
	LS_DATA_MISSING,	//Invalid as data is missing
	LS_KEY_MISMATCH,	//Invalid due to key mismatch
	LS_PROD_MISMATCH,	//Invalid, license is not for this product
	LS_MAC_MISMATCH,	//Invalid, license is not for this machine
	LS_INVALID,		//Invalid.
};

//This is the index for the array of pointers returned.
enum lic_data_indx {
	LDI_PROD_NAME,		//Product Name
	LDI_PROD_MAJ,		//Major version of product
	LDI_PROD_MIN,		//Minor version of product
	LDI_LIC_TYPE,		//License type
	LDI_DIFF_FROM,		//Current time - Validity start time
	LDI_DIFF_TO,		//Validity end time - Current time
	LDI_MAX_INDX,		//This has to be last field
};

/*
 * This data structure acts as language between license daemon and
 * license file. This has the version of file format and the state of
 * license (valid/expired/invalid). data field will have version specific
 * information. Its a list of pointers, the data each field points to 
 * is described in enum lic_data_indx.
 *
 * Which field will be set in this array depends on the version of 
 * the file.
 */
typedef struct lic_data {
	int		fld_major;		//Maj. version of file format
	int		fld_minor;		//Min.
	int		fld_state;		//License state
	void		*fld_data[LDI_MAX_INDX];//Array od data
} lic_data_t;

extern char *lic_state_msg[];
extern char *lic_installation_type[];
extern char *lic_period_type[];
	
/*
 * These are the interfaces exported to licensing daemon from the infrastructure.
 * 1. Generate blank license file based on version of file format.
 * 2. Generate license key based on the inputs in the file.
 * 3. Check whether the license is valid.
 * 4. Get license details.
 */
int	generate_license_file(char *, char *);
int	generate_license_for_file(char *, char *);
int	check_license_file(char *);
void 	get_license_details(char *, lic_data_t *);
#ifdef  __cplusplus
}
#endif
#endif
