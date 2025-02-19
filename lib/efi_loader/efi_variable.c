// SPDX-License-Identifier: GPL-2.0+
/*
 * UEFI runtime variable services
 *
 * Copyright (c) 2017 Rob Clark
 */

#define LOG_CATEGORY LOGC_EFI

#include <common.h>
#include <efi_loader.h>
#include <efi_variable.h>
#include <env.h>
#include <env_internal.h>
#include <hexdump.h>
#include <log.h>
#include <malloc.h>
#include <rtc.h>
#include <search.h>
#include <uuid.h>
#include <crypto/pkcs7_parser.h>
#include <linux/compat.h>
#include <u-boot/crc.h>
#include <asm/sections.h>

#ifdef CONFIG_EFI_SECURE_BOOT
static u8 pkcs7_hdr[] = {
	/* SEQUENCE */
	0x30, 0x82, 0x05, 0xc7,
	/* OID: pkcs7-signedData */
	0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02,
	/* Context Structured? */
	0xa0, 0x82, 0x05, 0xb8,
};

/**
 * efi_variable_parse_signature - parse a signature in variable
 * @buf:	Pointer to variable's value
 * @buflen:	Length of @buf
 * @tmpbuf:	Pointer to temporary buffer
 *
 * Parse a signature embedded in variable's value and instantiate
 * a pkcs7_message structure. Since pkcs7_parse_message() accepts only
 * pkcs7's signedData, some header needed be prepended for correctly
 * parsing authentication data, particularly for variable's.
 * A temporary buffer will be allocated if needed, and it should be
 * kept valid during the authentication because some data in the buffer
 * will be referenced by efi_signature_verify().
 *
 * Return:	Pointer to pkcs7_message structure on success, NULL on error
 */
static struct pkcs7_message *efi_variable_parse_signature(const void *buf,
							  size_t buflen,
							  u8 **tmpbuf)
{
	u8 *ebuf;
	size_t ebuflen, len;
	struct pkcs7_message *msg;

	/*
	 * This is the best assumption to check if the binary is
	 * already in a form of pkcs7's signedData.
	 */
	if (buflen > sizeof(pkcs7_hdr) &&
	    !memcmp(&((u8 *)buf)[4], &pkcs7_hdr[4], 11)) {
		msg = pkcs7_parse_message(buf, buflen);
		if (IS_ERR(msg))
			return NULL;
		return msg;
	}

	/*
	 * Otherwise, we should add a dummy prefix sequence for pkcs7
	 * message parser to be able to process.
	 * NOTE: EDK2 also uses similar hack in WrapPkcs7Data()
	 * in CryptoPkg/Library/BaseCryptLib/Pk/CryptPkcs7VerifyCommon.c
	 * TODO:
	 * The header should be composed in a more refined manner.
	 */
	EFI_PRINT("Makeshift prefix added to authentication data\n");
	ebuflen = sizeof(pkcs7_hdr) + buflen;
	if (ebuflen <= 0x7f) {
		EFI_PRINT("Data is too short\n");
		return NULL;
	}

	ebuf = malloc(ebuflen);
	if (!ebuf) {
		EFI_PRINT("Out of memory\n");
		return NULL;
	}

	memcpy(ebuf, pkcs7_hdr, sizeof(pkcs7_hdr));
	memcpy(ebuf + sizeof(pkcs7_hdr), buf, buflen);
	len = ebuflen - 4;
	ebuf[2] = (len >> 8) & 0xff;
	ebuf[3] = len & 0xff;
	len = ebuflen - 0x13;
	ebuf[0x11] = (len >> 8) & 0xff;
	ebuf[0x12] = len & 0xff;

	msg = pkcs7_parse_message(ebuf, ebuflen);

	if (IS_ERR(msg)) {
		free(ebuf);
		return NULL;
	}

	*tmpbuf = ebuf;
	return msg;
}

/**
 * efi_variable_authenticate - authenticate a variable
 * @variable:	Variable name in u16
 * @vendor:	Guid of variable
 * @data_size:	Size of @data
 * @data:	Pointer to variable's value
 * @given_attr:	Attributes to be given at SetVariable()
 * @env_attr:	Attributes that an existing variable holds
 * @time:	signed time that an existing variable holds
 *
 * Called by efi_set_variable() to verify that the input is correct.
 * Will replace the given data pointer with another that points to
 * the actual data to store in the internal memory.
 * On success, @data and @data_size will be replaced with variable's
 * actual data, excluding authentication data, and its size, and variable's
 * attributes and signed time will also be returned in @env_attr and @time,
 * respectively.
 *
 * Return:	status code
 */
static efi_status_t efi_variable_authenticate(u16 *variable,
					      const efi_guid_t *vendor,
					      efi_uintn_t *data_size,
					      const void **data, u32 given_attr,
					      u32 *env_attr, u64 *time)
{
	const struct efi_variable_authentication_2 *auth;
	struct efi_signature_store *truststore, *truststore2;
	struct pkcs7_message *var_sig;
	struct efi_image_regions *regs;
	struct efi_time timestamp;
	struct rtc_time tm;
	u64 new_time;
	u8 *ebuf;
	enum efi_auth_var_type var_type;
	efi_status_t ret;

	var_sig = NULL;
	truststore = NULL;
	truststore2 = NULL;
	regs = NULL;
	ebuf = NULL;
	ret = EFI_SECURITY_VIOLATION;

	if (*data_size < sizeof(struct efi_variable_authentication_2))
		goto err;

	/* authentication data */
	auth = *data;
	if (*data_size < (sizeof(auth->time_stamp)
				+ auth->auth_info.hdr.dwLength))
		goto err;

	if (guidcmp(&auth->auth_info.cert_type, &efi_guid_cert_type_pkcs7))
		goto err;

	memcpy(&timestamp, &auth->time_stamp, sizeof(timestamp));
	if (timestamp.pad1 || timestamp.nanosecond || timestamp.timezone ||
	    timestamp.daylight || timestamp.pad2)
		goto err;

	*data += sizeof(auth->time_stamp) + auth->auth_info.hdr.dwLength;
	*data_size -= (sizeof(auth->time_stamp)
				+ auth->auth_info.hdr.dwLength);

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = timestamp.year;
	tm.tm_mon = timestamp.month;
	tm.tm_mday = timestamp.day;
	tm.tm_hour = timestamp.hour;
	tm.tm_min = timestamp.minute;
	tm.tm_sec = timestamp.second;
	new_time = rtc_mktime(&tm);

	if (!efi_secure_boot_enabled()) {
		/* finished checking */
		*time = new_time;
		return EFI_SUCCESS;
	}

	if (new_time <= *time)
		goto err;

	/* data to be digested */
	regs = calloc(sizeof(*regs) + sizeof(struct image_region) * 5, 1);
	if (!regs)
		goto err;
	regs->max = 5;
	efi_image_region_add(regs, (uint8_t *)variable,
			     (uint8_t *)variable
				+ u16_strlen(variable) * sizeof(u16), 1);
	efi_image_region_add(regs, (uint8_t *)vendor,
			     (uint8_t *)vendor + sizeof(*vendor), 1);
	efi_image_region_add(regs, (uint8_t *)&given_attr,
			     (uint8_t *)&given_attr + sizeof(given_attr), 1);
	efi_image_region_add(regs, (uint8_t *)&timestamp,
			     (uint8_t *)&timestamp + sizeof(timestamp), 1);
	efi_image_region_add(regs, (uint8_t *)*data,
			     (uint8_t *)*data + *data_size, 1);

	/* variable's signature list */
	if (auth->auth_info.hdr.dwLength < sizeof(auth->auth_info))
		goto err;

	/* ebuf should be kept valid during the authentication */
	var_sig = efi_variable_parse_signature(auth->auth_info.cert_data,
					       auth->auth_info.hdr.dwLength
						   - sizeof(auth->auth_info),
					       &ebuf);
	if (!var_sig) {
		EFI_PRINT("Parsing variable's signature failed\n");
		goto err;
	}

	/* signature database used for authentication */
	var_type = efi_auth_var_get_type(variable, vendor);
	switch (var_type) {
	case EFI_AUTH_VAR_PK:
	case EFI_AUTH_VAR_KEK:
		/* with PK */
		truststore = efi_sigstore_parse_sigdb(L"PK");
		if (!truststore)
			goto err;
		break;
	case EFI_AUTH_VAR_DB:
	case EFI_AUTH_VAR_DBX:
		/* with PK and KEK */
		truststore = efi_sigstore_parse_sigdb(L"KEK");
		truststore2 = efi_sigstore_parse_sigdb(L"PK");
		if (!truststore) {
			if (!truststore2)
				goto err;

			truststore = truststore2;
			truststore2 = NULL;
		}
		break;
	default:
		/* TODO: support private authenticated variables */
		goto err;
	}

	/* verify signature */
	if (efi_signature_verify(regs, var_sig, truststore, NULL)) {
		EFI_PRINT("Verified\n");
	} else {
		if (truststore2 &&
		    efi_signature_verify(regs, var_sig, truststore2, NULL)) {
			EFI_PRINT("Verified\n");
		} else {
			EFI_PRINT("Verifying variable's signature failed\n");
			goto err;
		}
	}

	/* finished checking */
	*time = new_time;
	ret = EFI_SUCCESS;

err:
	efi_sigstore_free(truststore);
	efi_sigstore_free(truststore2);
	pkcs7_free_message(var_sig);
	free(ebuf);
	free(regs);

	return ret;
}
#else
static inline efi_status_t efi_variable_authenticate(u16 *variable, const efi_guid_t *vendor,
						     efi_uintn_t *data_size, const void **data,
						     u32 given_attr, u32 *env_attr, u64 *time)
{
	return EFI_SUCCESS;
}
#endif /* CONFIG_EFI_SECURE_BOOT */

efi_status_t __efi_runtime
efi_get_variable_int(u16 *variable_name, const efi_guid_t *vendor,
		     u32 *attributes, efi_uintn_t *data_size, void *data,
		     u64 *timep)
{
	return efi_get_variable_mem(variable_name, vendor, attributes, data_size, data, timep);
}

efi_status_t __efi_runtime
efi_get_next_variable_name_int(efi_uintn_t *variable_name_size,
			       u16 *variable_name, efi_guid_t *vendor)
{
	return efi_get_next_variable_name_mem(variable_name_size, variable_name, vendor);
}

efi_status_t __efi_runtime
efi_set_variable_int(u16 *variable_name, const efi_guid_t *vendor,
		     u32 attributes, efi_uintn_t data_size,
		     const void *data, bool ro_check)
{
	struct efi_var_entry *var;
	efi_uintn_t ret;
	bool append, delete;
	u64 time = 0;
	enum efi_auth_var_type var_type;

	if (!variable_name || !*variable_name || !vendor ||
	    ((attributes & EFI_VARIABLE_RUNTIME_ACCESS) &&
	     !(attributes & EFI_VARIABLE_BOOTSERVICE_ACCESS)))
		return EFI_INVALID_PARAMETER;

	/* check if a variable exists */
	var = efi_var_mem_find(vendor, variable_name, NULL);
	append = !!(attributes & EFI_VARIABLE_APPEND_WRITE);
	attributes &= ~(u32)EFI_VARIABLE_APPEND_WRITE;
	delete = !append && (!data_size || !attributes);

	/* check attributes */
	var_type = efi_auth_var_get_type(variable_name, vendor);
	if (var) {
		if (ro_check && (var->attr & EFI_VARIABLE_READ_ONLY))
			return EFI_WRITE_PROTECTED;

		if (IS_ENABLED(CONFIG_EFI_VARIABLES_PRESEED)) {
			if (var_type != EFI_AUTH_VAR_NONE)
				return EFI_WRITE_PROTECTED;
		}

		/* attributes won't be changed */
		if (!delete &&
		    ((ro_check && var->attr != attributes) ||
		     (!ro_check && ((var->attr & ~(u32)EFI_VARIABLE_READ_ONLY)
				    != (attributes & ~(u32)EFI_VARIABLE_READ_ONLY))))) {
			return EFI_INVALID_PARAMETER;
		}
		time = var->time;
	} else {
		if (delete || append)
			/*
			 * Trying to delete or to update a non-existent
			 * variable.
			 */
			return EFI_NOT_FOUND;
	}

	if (var_type != EFI_AUTH_VAR_NONE) {
		/* authentication is mandatory */
		if (!(attributes &
		      EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS)) {
			EFI_PRINT("%ls: TIME_BASED_AUTHENTICATED_WRITE_ACCESS required\n",
				  variable_name);
			return EFI_INVALID_PARAMETER;
		}
	}

	/* authenticate a variable */
	if (IS_ENABLED(CONFIG_EFI_SECURE_BOOT)) {
		if (attributes & EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS)
			return EFI_INVALID_PARAMETER;
		if (attributes &
		    EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS) {
			u32 env_attr;

			ret = efi_variable_authenticate(variable_name, vendor,
							&data_size, &data,
							attributes, &env_attr,
							&time);
			if (ret != EFI_SUCCESS)
				return ret;

			/* last chance to check for delete */
			if (!data_size)
				delete = true;
		}
	} else {
		if (attributes &
		    (EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS |
		     EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS)) {
			EFI_PRINT("Secure boot is not configured\n");
			return EFI_INVALID_PARAMETER;
		}
	}

	if (delete) {
		/* EFI_NOT_FOUND has been handled before */
		ret = EFI_SUCCESS;
	} else if (append) {
		u16 *old_data = var->name;

		for (; *old_data; ++old_data)
			;
		++old_data;
		ret = efi_var_mem_ins(variable_name, vendor, attributes,
				      var->length, old_data, data_size, data,
				      time);
	} else {
		ret = efi_var_mem_ins(variable_name, vendor, attributes,
				      data_size, data, 0, NULL, time);
	}
	efi_var_mem_del(var);

	if (ret != EFI_SUCCESS)
		return ret;

	if (var_type == EFI_AUTH_VAR_PK)
		ret = efi_init_secure_state();
	else
		ret = EFI_SUCCESS;

	/* Write non-volatile EFI variables to file */
	if ((attributes & EFI_VARIABLE_NON_VOLATILE &&
	     ret == EFI_SUCCESS) || delete)
		ret = efi_var_to_file();

	return ret;
}

efi_status_t efi_query_variable_info_int(u32 attributes,
					 u64 *maximum_variable_storage_size,
					 u64 *remaining_variable_storage_size,
					 u64 *maximum_variable_size)
{
	*maximum_variable_storage_size = EFI_VAR_BUF_SIZE -
					 sizeof(struct efi_var_file);
	*remaining_variable_storage_size = efi_var_mem_free();
	*maximum_variable_size = EFI_VAR_BUF_SIZE -
				 sizeof(struct efi_var_file) -
				 sizeof(struct efi_var_entry);
	return EFI_SUCCESS;
}

/**
 * efi_query_variable_info_runtime() - runtime implementation of
 *				       QueryVariableInfo()
 *
 * @attributes:				bitmask to select variables to be
 *					queried
 * @maximum_variable_storage_size:	maximum size of storage area for the
 *					selected variable types
 * @remaining_variable_storage_size:	remaining size of storage are for the
 *					selected variable types
 * @maximum_variable_size:		maximum size of a variable of the
 *					selected type
 * Returns:				status code
 */
efi_status_t __efi_runtime EFIAPI
efi_query_variable_info_runtime(u32 attributes, u64 *maximum_variable_storage_size,
				u64 *remaining_variable_storage_size,
				u64 *maximum_variable_size)
{
	return EFI_UNSUPPORTED;
}

/**
 * efi_set_variable_runtime() - runtime implementation of SetVariable()
 *
 * @variable_name:	name of the variable
 * @vendor:		vendor GUID
 * @attributes:		attributes of the variable
 * @data_size:		size of the buffer with the variable value
 * @data:		buffer with the variable value
 * Return:		status code
 */
efi_status_t __efi_runtime EFIAPI
efi_set_variable_runtime(u16 *variable_name, const efi_guid_t *vendor,
			 u32 attributes, efi_uintn_t data_size,
			 const void *data)
{
	efi_status_t ret;

	/* Make sure that EFI_VARIABLE_READ_ONLY flasg is not set */
	if (attributes & ~(u32)EFI_VARIABLE_MASK)
		ret = EFI_INVALID_PARAMETER;
	else
		ret = efi_set_variable_int(variable_name, vendor, attributes,
					   data_size, data, true);
	return ret;
}

/**
 * efi_variables_boot_exit_notify() - notify ExitBootServices() is called
 */
void efi_variables_boot_exit_notify(void)
{
	/* Switch variable services functions to runtime version */
	efi_runtime_services.get_variable = efi_get_variable_runtime;
	efi_runtime_services.get_next_variable_name =
				efi_get_next_variable_name_runtime;
	efi_runtime_services.set_variable = efi_set_variable_runtime;
	efi_runtime_services.query_variable_info =
				efi_query_variable_info_runtime;
	efi_update_table_header_crc32(&efi_runtime_services.hdr);
}

/**
 * efi_init_variables() - initialize variable services
 *
 * Return:	status code
 */
efi_status_t efi_init_variables(void)
{
	efi_status_t ret;
	u32 mtc;
	efi_uintn_t mtc_size = 4;

	ret = efi_var_mem_init();
	if (ret != EFI_SUCCESS)
		return ret;

	if (IS_ENABLED(CONFIG_EFI_VARIABLES_PRESEED)) {
		ret = efi_var_restore((struct efi_var_file *)
				      __efi_var_file_begin);
		if (ret != EFI_SUCCESS)
			log_err("Invalid EFI variable seed\n");
	}

	ret = efi_var_from_file();
	if (ret != EFI_SUCCESS)
		return ret;

	/* Initialize high 32-bit monotonic counter */
	ret = efi_get_variable_int(L"MTC", &efi_global_variable_guid, NULL,
				   &mtc_size, &mtc, NULL);
	/* Does not exist, initialize count to zero */
	if (ret) {
		mtc = 0;
		log_info("Init Monotonic Count to zero\n");
	}

	/* Increment for current reset */
	mtc++;
	ret = efi_set_variable_int(L"MTC", &efi_global_variable_guid,
				   EFI_VARIABLE_RUNTIME_ACCESS |
				   EFI_VARIABLE_NON_VOLATILE |
				   EFI_VARIABLE_BOOTSERVICE_ACCESS,
				   mtc_size, &mtc, false);
	if (ret)
		log_err("Monotonic Count variable not set\n");

	return efi_init_secure_state();
}
