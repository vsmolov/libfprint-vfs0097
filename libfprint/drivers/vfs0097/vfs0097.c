/*
 * Validity VFS0097 driver for libfprint
 * Copyright (C) 2017 Nikita Mikhailov <nikita.s.mikhailov@gmail.com>
 * Copyright (C) 2018 Marco Trevisan <marco@ubuntu.com>
 * Copyright (C) 2020 Viktor Smolov <smolovv@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "vfs0097"

#include <nss.h>
#include <stdio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <ssl.h>

#include "drivers_api.h"

#include "fpi-byte-reader.h"

#include "vfs0097.h"

#define EP_IN (1 | FPI_USB_ENDPOINT_IN)
#define EP_OUT (1 | FPI_USB_ENDPOINT_OUT)
#define EP_INTERRUPT (3 | FPI_USB_ENDPOINT_IN)

G_DEFINE_TYPE (FpiDeviceVfs0097, fpi_device_vfs0097, FP_TYPE_DEVICE)

void
print_hex (const char *buffer, size_t size)
{
  char *result = g_malloc0 (size * 2 + 1);

  for (size_t i = 0; i < size; i++)
    sprintf (result + i * 2, "%02x", buffer[i] & 0xff);
  result[size * 2] = 0;

  fp_info ("%s", result);

  g_free (result);
}

/* Usb id table of device */
static const FpIdEntry id_table[] = {
  {.vid = 0x138a,  .pid = 0x0097, },
  {.vid = 0,  .pid = 0,  .driver_data = 0},
};

/* USB functions */

/* Callback for async_write */
static void
async_write_callback (FpiUsbTransfer *transfer, FpDevice *device,
                      gpointer user_data, GError *error)
{
  if (error)
    {
      fp_err ("USB write transfer: %s", error->message);
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  fpi_ssm_next_state (transfer->ssm);
}

/* Send data to EP1, the only out endpoint */
static void
async_write (FpiSsm   *ssm,
             FpDevice *dev,
             void     *data,
             int       len)
{
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (FP_DEVICE (dev));
  fpi_usb_transfer_fill_bulk_full (transfer, EP_OUT, data, len, NULL);
  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (transfer, VFS_USB_TIMEOUT, NULL,
                           async_write_callback, NULL);
}

/* Callback for async_read */
static void
async_read_callback (FpiUsbTransfer *transfer, FpDevice *device,
                     gpointer user_data, GError *error)
{
  if (error)
    {
      fp_err ("USB read transfer on endpoint %d: %s", transfer->endpoint, error->message);
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (user_data)
    *((gsize *) user_data) = transfer->actual_length;
  fpi_ssm_next_state (transfer->ssm);
}

/* Receive data from the given ep and either discard or fill the given buffer */
static void
async_read (FpiSsm   *ssm,
            FpDevice *dev,
            void     *data,
            gsize     len,
            gsize    *actual_length)
{
  FpiUsbTransfer *transfer;
  GDestroyNotify free_func = NULL;

  transfer = fpi_usb_transfer_new (FP_DEVICE (dev));
  transfer->ssm = ssm;
  transfer->short_is_error = FALSE; // TODO: We do not know actual response lengths yet, so

  if (data == NULL)
    {
      data = g_malloc0 (len);
      free_func = g_free;
    }

  fpi_usb_transfer_fill_bulk_full (transfer, EP_IN, data, len, free_func);

  fpi_usb_transfer_submit (transfer, VFS_USB_TIMEOUT, NULL,
                           async_read_callback, actual_length);
}
/* Image processing functions */

/* Proto functions */
struct command_ssm_data_t
{
  guchar *buffer;
  gssize  length;
};

static guint8 *
HMAC_SHA256 (const guint8 *key, guint32 key_len,
             const guint8 *data, guint32 data_len,
             guint8 *result)
{
  guint32 unused;

  return HMAC (EVP_sha256 (), key, key_len, data, data_len, result, &unused);
}

static void
PRF_SHA256 (const guint8 *secret, guint32 secret_len,
            const guint8 *label, guint32 label_len,
            const guint8 *seed, guint32 seed_len, guint8 *out, guint32 len)
{
  gsize size;
  gsize pos;
  guint8 P[SHA256_DIGEST_LENGTH];
  guint8 *A;

  /*
   * RFC 5246, Chapter 5
   * A(0) = lseed
   * A(i) = HMAC_hash(secret, A(i-1))
   *
   * P_hash(secret, lseed) = HMAC_hash(secret, A(1) + lseed) +
   *                         HMAC_hash(secret, A(2) + lseed) +
   *                         HMAC_hash(secret, A(3) + lseed) + ...
   *
   * PRF(secret, label, seed) = P_hash(secret, label + seed)
   */

  // A(0)
  A = g_malloc0 (SHA256_DIGEST_LENGTH + label_len + seed_len);
  memcpy (A, label, label_len);
  memcpy (A + label_len, seed, seed_len);

  // A(1)
  HMAC_SHA256 (secret, secret_len, A, label_len + seed_len, A);

  pos = 0;
  while (pos < len)
    {
      // Concatenate A + label + seed
      memcpy (A + SHA256_DIGEST_LENGTH, label, label_len);
      memcpy (A + SHA256_DIGEST_LENGTH + label_len, seed, seed_len);

      // Calculate new P_hash part
      HMAC_SHA256 (secret, secret_len, A, SHA256_DIGEST_LENGTH + label_len + seed_len, P);

      // Calculate next A
      HMAC_SHA256 (secret, secret_len, A, SHA256_DIGEST_LENGTH, A);

      size = MIN (len - pos, SHA256_DIGEST_LENGTH);
      memcpy (out + pos, P, size);
      pos += size;
    }

  g_free (A);
}

static void
init_private_key (FpiDeviceVfs0097 *self, const guint8 *body, guint16 size)
{
  char AES_MASTER_KEY[SHA256_DIGEST_LENGTH];
  char VALIDATION_KEY[SHA256_DIGEST_LENGTH];

  PRF_SHA256 (PRE_KEY, G_N_ELEMENTS (PRE_KEY),
              LABEL, G_N_ELEMENTS (LABEL),
              self->seed, self->seed_length,
              AES_MASTER_KEY, SHA256_DIGEST_LENGTH);

  PRF_SHA256 (AES_MASTER_KEY, SHA256_DIGEST_LENGTH,
              LABEL_SIGN, G_N_ELEMENTS (LABEL_SIGN),
              SIGN_KEY, G_N_ELEMENTS (SIGN_KEY),
              VALIDATION_KEY, SHA256_DIGEST_LENGTH);

  const guint8 prefix = body[0];
  if (prefix != 2)
    {
      fp_warn ("Unknown private key prefix %02x", prefix);
      return;
    }

  const guint8 *encrypted = &body[1];
  const guint8 *hash = &body[size - SHA256_DIGEST_LENGTH];

  guint8 calc_hash[SHA256_DIGEST_LENGTH];
  HMAC_SHA256 (VALIDATION_KEY, SHA256_DIGEST_LENGTH, encrypted, size - 1 - SHA256_DIGEST_LENGTH, calc_hash);

  if (memcmp (calc_hash, hash, SHA256_DIGEST_LENGTH) != 0)
    {
      fp_warn ("Signature verification failed. This device was probably paired with another computer.");
      return;
    }

  EVP_CIPHER_CTX *context;
  context = EVP_CIPHER_CTX_new ();
  unsigned char *decrypted = NULL;
  int tlen1 = 0, tlen2;

  if (!EVP_DecryptInit (context, EVP_aes_256_cbc (), AES_MASTER_KEY, encrypted))
    {
      fp_err ("Failed to initialize EVP decrypt, error: %lu, %s",
              ERR_peek_last_error (), ERR_error_string (ERR_peek_last_error (), NULL));
      return;
    }

  decrypted = g_malloc (0x70);
  EVP_CIPHER_CTX_set_padding (context, 0);

  if (!EVP_DecryptUpdate (context, decrypted, &tlen1, encrypted + 0x10, 0x70))
    {
      fp_err ("Failed to EVP decrypt, error: %lu, %s",
              ERR_peek_last_error (), ERR_error_string (ERR_peek_last_error (), NULL));
      return;
    }

  if (!EVP_DecryptFinal (context, decrypted + tlen1, &tlen2))
    {
      fp_err ("EVP Final decrypt failed, error: %lu, %s",
              ERR_peek_last_error (), ERR_error_string (ERR_peek_last_error (), NULL));
      return;
    }

  EVP_CIPHER_CTX_free (context);

  BIGNUM *x = BN_lebin2bn (decrypted, 0x20, NULL);
  BIGNUM *y = BN_lebin2bn (decrypted + 0x20, 0x20, NULL);
  BIGNUM *d = BN_lebin2bn (decrypted + 0x40, 0x20, NULL);

  EC_KEY *key = EC_KEY_new_by_curve_name (NID_X9_62_prime256v1);

  if (!EC_KEY_set_public_key_affine_coordinates (key, x, y))
    {
      fp_err ("Failed to set public key coordinates, error: %lu, %s",
              ERR_peek_last_error (), ERR_error_string (ERR_peek_last_error (), NULL));
      return;
    }

  if (!EC_KEY_set_private_key (key, d))
    {
      fp_err ("Failed to set private key, error: %lu, %s",
              ERR_peek_last_error (), ERR_error_string (ERR_peek_last_error (), NULL));

      return;
    }

  if (!EC_KEY_check_key (key))
    {
      fp_err ("Failed to check key, error: %lu, %s",
              ERR_peek_last_error (), ERR_error_string (ERR_peek_last_error (), NULL));
      return;
    }

  self->private_key = key;

  fp_dbg ("X: %s", BN_bn2hex (x));
  fp_dbg ("Y: %s", BN_bn2hex (y));
  fp_dbg ("D: %s", BN_bn2hex (d));

  g_clear_pointer (&decrypted, g_free);
  g_clear_pointer (&x, BN_free);
  g_clear_pointer (&y, BN_free);
  g_clear_pointer (&d, BN_free);
}

static void
check_ecdh (FpiDeviceVfs0097 *self, const guint8 * body, guint16 size)
{
  FpiByteReader *reader;
  const guint8 *xb;
  const guint8 *yb;
  const guint16 KEY_SIZE = 0x90;

  reader = fpi_byte_reader_new (body, size);

  fpi_byte_reader_set_pos (reader, 0x08);
  fpi_byte_reader_get_data (reader, 0x20, &xb);
  fpi_byte_reader_set_pos (reader, 0x4c);
  fpi_byte_reader_get_data (reader, 0x20, &yb);

  BIGNUM *x = BN_lebin2bn (xb, 0x20, NULL);
  BIGNUM *y = BN_lebin2bn (yb, 0x20, NULL);

  EC_KEY *key = EC_KEY_new_by_curve_name (NID_X9_62_prime256v1);

  if (!EC_KEY_set_public_key_affine_coordinates (key, x, y))
    {
      fp_err ("Failed to set public key coordinates, error: %lu, %s",
              ERR_peek_last_error (), ERR_error_string (ERR_peek_last_error (), NULL));
      return;
    }

  fp_dbg ("ECDH X: %s", BN_bn2hex (x));
  fp_dbg ("ECDH Y: %s", BN_bn2hex (y));

  g_clear_pointer (&x, BN_free);
  g_clear_pointer (&y, BN_free);

  self->ecdh_q = key;

  const guint8 *signature;
  guint32 signature_length;

  fpi_byte_reader_set_pos (reader, KEY_SIZE);

  fpi_byte_reader_get_uint32_le (reader, &signature_length);
  fpi_byte_reader_get_data (reader, signature_length, &signature);

  while (fpi_byte_reader_get_remaining (reader))
    {
      guint8 b;
      fpi_byte_reader_get_uint8 (reader, &b);
      if (b != 0)
        fp_warn ("Expected zero at %d", fpi_byte_reader_get_pos (reader));
    }


  x = BN_bin2bn (DEVICE_KEY_X, 0x20, NULL);
  y = BN_bin2bn (DEVICE_KEY_Y, 0x20, NULL);

  EC_KEY *device_key = EC_KEY_new_by_curve_name (NID_X9_62_prime256v1);

  if (!EC_KEY_set_public_key_affine_coordinates (device_key, x, y))
    {
      fp_err ("Failed to set public key coordinates, error: %lu, %s",
              ERR_peek_last_error (), ERR_error_string (ERR_peek_last_error (), NULL));
      return;
    }

  guint8 dgst[SHA256_DIGEST_LENGTH];
  SHA256 (body, KEY_SIZE, dgst);

  int verify_status = ECDSA_verify (0, dgst, SHA256_DIGEST_LENGTH, signature, signature_length, device_key);
  if (verify_status == 0)
    fp_err ("Untrusted device");
  else if (verify_status < 0)
    fp_err ("Failed to verify signature, error: %lu, %s",
            ERR_peek_last_error (), ERR_error_string (ERR_peek_last_error (), NULL));

  g_clear_pointer (&reader, fpi_byte_reader_free);
  g_clear_pointer (&device_key, EC_KEY_free);
  g_clear_pointer (&x, BN_free);
  g_clear_pointer (&y, BN_free);
}

static void
init_certificate (FpiDeviceVfs0097 *self, const guint8 * body, guint16 size)
{
  self->certificate_length = size;
  self->certificate = g_malloc0 (size);
  memcpy (self->certificate, body, size);
}

static void
init_keys (FpDevice *dev)
{
  FpiByteReader reader;
  FpiDeviceVfs0097 *self = FPI_DEVICE_VFS0097 (dev);
  guint32 size;

  fpi_byte_reader_init (&reader, self->buffer, self->buffer_length);
  fpi_byte_reader_skip (&reader, 2);
  fpi_byte_reader_get_uint32_le (&reader, &size);
  fpi_byte_reader_skip (&reader, 2);

  g_assert (fpi_byte_reader_get_remaining (&reader) == size);

  guint16 id, body_size;
  const guint8 *hash;
  const guint8 *body;

  while (fpi_byte_reader_get_remaining_inline (&reader) > 0)
    {
      fpi_byte_reader_get_uint16_le (&reader, &id);
      fpi_byte_reader_get_uint16_le (&reader, &body_size);

      if (id == 0xffff)
        break;

      fpi_byte_reader_get_data (&reader, SHA256_DIGEST_LENGTH, &hash);
      fpi_byte_reader_get_data (&reader, body_size, &body);

      guint8 calc_hash[SHA256_DIGEST_LENGTH];
      SHA256 (body, body_size, calc_hash);

      if (memcmp (calc_hash, hash, SHA256_DIGEST_LENGTH) != 0)
        {
          fp_warn ("Hash mismatch for block %d", id);
          continue;
        }

      switch (id)
        {
        case 0:
        case 1:
        case 2:
          // All zeros
          break;

        case 3:
          init_certificate (self, body, body_size);
          break;

        case 4:
          init_private_key (self, body, body_size);
          break;

        case 6:
          check_ecdh (self, body, body_size);
          break;

        default:
          fp_warn ("Unhandled block id %04x (%d bytes)", id, body_size);
          break;
        }
    }
}

/* SSM loop for exec_command */
static void
exec_command_ssm (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceVfs0097 *self = FPI_DEVICE_VFS0097 (dev);
  struct command_ssm_data_t *data = fpi_ssm_get_data (ssm);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case EXEC_COMMAND_SM_WRITE:
      async_write (ssm, dev, data->buffer, data->length);
      break;

    case EXEC_COMMAND_SM_READ:
      async_read (ssm, dev, self->buffer, VFS_USB_BUFFER_SIZE, &self->buffer_length);
      break;

    default:
      fp_err ("Unknown EXEC_COMMAND_SM state");
      fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
    }
}

/* Send command and read response */
static void
exec_command (FpDevice *dev, FpiSsm *ssm, const guchar *buffer, gsize length)
{
  struct command_ssm_data_t *data;
  FpiSsm *subsm;

  data = g_new0 (struct command_ssm_data_t, 1);
  data->buffer = (guchar *) buffer;
  data->length = length;

  subsm = fpi_ssm_new (dev, exec_command_ssm, EXEC_COMMAND_SM_STATES);
  fpi_ssm_set_data (subsm, data, g_free);

  fpi_ssm_start_subsm (ssm, subsm);
}

/* SSM loop for TLS handshake */
static void
handshake_ssm (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
  {
    case TLS_HANDSHAKE_SM_CLIENT_HELLO:
//      exec_command(dev, ssm, "", 0);


      fpi_ssm_next_state(ssm);
      break;

    case TLS_HANDSHAKE_SM_GENERATE_CERTIFICATE:
      fpi_ssm_next_state(ssm);
      break;

    case TLS_HANDSHAKE_SM_CLIENT_FINISHED:
      fpi_ssm_next_state(ssm);
      break;

    default:
      fp_err ("Unknown EXEC_COMMAND_SM state");
      fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
  }
}

static void
do_handshake (FpDevice *dev, FpiSsm *ssm)
{
  FpiSsm *subsm;

  subsm = fpi_ssm_new (dev, handshake_ssm, TLS_HANDSHAKE_STATES);
  fpi_ssm_start_subsm (ssm, subsm);
}

/* Device functions */

/* SSM loop for device initialization */
static void
init_ssm (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceVfs0097 *self = FPI_DEVICE_VFS0097 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SEND_INIT_1:
      exec_command (dev, ssm, INIT_SEQUENCE_MSG1, G_N_ELEMENTS (INIT_SEQUENCE_MSG1));
      break;

    case CHECK_INITIALIZED:
      if (self->buffer_length == 38)
        {
          if (self->buffer[self->buffer_length - 1] != 0x07)
            {
              fp_err ("Sensor is not initialized, init byte is 0x%02x "
                      "(should be 0x07 on initialized devices, 0x02 otherwise)\n" \
                      "This is a driver in alpha state and the device needs to be setup in a VirtualBox " \
                      "instance running Windows, or with a native Windows installation first.",
                      self->buffer[self->buffer_length - 1]);
              fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                                  "Device is not initialized"));
              break;
            }
        }
      else
        {
          fp_warn ("Unknown reply at init");
          break;
        }

    case SEND_INIT_2:
      exec_command (dev, ssm, INIT_SEQUENCE_MSG2, G_N_ELEMENTS (INIT_SEQUENCE_MSG2));
      break;

    case GET_PARTITION_HEADER:
      exec_command (dev, ssm, INIT_SEQUENCE_MSG3, G_N_ELEMENTS (INIT_SEQUENCE_MSG3));
      break;

    case SEND_INIT_4:
      exec_command (dev, ssm, INIT_SEQUENCE_MSG4, G_N_ELEMENTS (INIT_SEQUENCE_MSG4));
      break;

    case GET_FLASH_INFO:
      exec_command (dev, ssm, INIT_SEQUENCE_MSG5, G_N_ELEMENTS (INIT_SEQUENCE_MSG5));
      break;

    case READ_FLASH_TLS_DATA:
      exec_command (dev, ssm, INIT_SEQUENCE_MSG6, G_N_ELEMENTS (INIT_SEQUENCE_MSG6));
      break;

    case INIT_KEYS:
      init_keys (dev);
      fpi_ssm_next_state (ssm);
      break;

    case HANDSHAKE:
      do_handshake (dev, ssm);
      break;

    default:
      fp_err ("Unknown INIT_SM state");
      fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_PROTO));
    }
}

/* Clears all fprint data */
static void
clear_data (FpiDeviceVfs0097 *self)
{
  g_clear_pointer (&self->seed, g_free);
  g_clear_pointer (&self->buffer, g_free);
  g_clear_pointer (&self->certificate, g_free);
  g_clear_pointer (&self->private_key, EC_KEY_free);
  g_clear_pointer (&self->ecdh_q, EC_KEY_free);
}

/* Callback for device initialization SSM */
static void
dev_open_callback (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  /* Notify open complete */
  fpi_device_open_complete (dev, error);
}

/* Open device */
static void
dev_open (FpDevice *device)
{
  GError *error = NULL;
  FpiDeviceVfs0097 *self = FPI_DEVICE_VFS0097 (device);

  GUsbDevice *usb_dev;
  gint config;
  SECStatus rv;

  if (!self->seed)
    {
      error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                        "Seed value is not initialized");
      fpi_device_open_complete (FP_DEVICE (self), error);
      return;
    }

  /* Claim usb interface */
  usb_dev = fpi_device_get_usb_device (device);
  if (!g_usb_device_reset (usb_dev, &error))
    {
      fpi_device_open_complete (FP_DEVICE (self), error);
      return;
    }

  config = g_usb_device_get_configuration (usb_dev, &error);
  if (config < 0)
    {
      fpi_device_open_complete (FP_DEVICE (self), error);
      return;
    }
  else if (config == 0)
    {
      g_usb_device_set_configuration (usb_dev, 1, &error);
    }

  if (!g_usb_device_claim_interface (usb_dev, 0, 0, &error))
    {
      fpi_device_open_complete (FP_DEVICE (self), error);
      return;
    }

  /* Initialise NSS early */
  rv = NSS_NoDB_Init (".");
  if (rv != SECSuccess)
    {
      fp_err ("Could not initialize NSS");
      error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                        "Could not initialize NSS");
      fpi_device_open_complete (FP_DEVICE (self), error);
      return;
    }

  self->buffer = g_malloc0 (VFS_USB_BUFFER_SIZE);

  FpiSsm *ssm = fpi_ssm_new (FP_DEVICE (self), init_ssm, INIT_SM_STATES);
  fpi_ssm_start (ssm, dev_open_callback);
}

/* Close device */
static void
dev_close (FpDevice *device)
{
  FpiDeviceVfs0097 *self = FPI_DEVICE_VFS0097 (device);
  GError *error = NULL;

  clear_data (self);

  /* Release usb interface */
  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (self)),
                                  0, 0, &error);

  /* Notify close complete */
  fpi_device_close_complete (FP_DEVICE (self), error);
}

/* List prints */
static void
dev_list (FpDevice *device)
{
  FpiDeviceVfs0097 *self = FPI_DEVICE_VFS0097 (device);

  G_DEBUG_HERE ();

  self->list_result = g_ptr_array_new_with_free_func (g_object_unref);

  fpi_device_list_complete (FP_DEVICE (self),
                            g_steal_pointer (&self->list_result),
                            NULL);
}

/* List prints */
static void
dev_enroll (FpDevice *device)
{
  FpiDeviceVfs0097 *self = FPI_DEVICE_VFS0097 (device);
  FpPrint *print = NULL;

  G_DEBUG_HERE ();

  fpi_device_get_enroll_data (device, &print);

  fpi_device_enroll_complete (FP_DEVICE (self), g_object_ref (print), NULL);
}

/* Delete print */
static void
dev_delete (FpDevice *device)
{
  FpiDeviceVfs0097 *self = FPI_DEVICE_VFS0097 (device);

  G_DEBUG_HERE ();

  fpi_device_delete_complete (FP_DEVICE (self), NULL);
}

/* Identify print */
static void
dev_identify (FpDevice *device)
{
  FpiDeviceVfs0097 *self = FPI_DEVICE_VFS0097 (device);

  G_DEBUG_HERE ();

  fpi_device_identify_complete (FP_DEVICE (self), NULL);
}

/* Verify print */
static void
dev_verify (FpDevice *device)
{
  FpiDeviceVfs0097 *self = FPI_DEVICE_VFS0097 (device);
  FpPrint *print = NULL;

  G_DEBUG_HERE ();

//  fpi_device_get_verify_data (device, &print);
//  g_debug ("username: %s", fp_print_get_username(print));
//  fpi_device_verify_report (device, FPI_MATCH_SUCCESS, NULL, NULL);

  fpi_device_verify_complete (FP_DEVICE (self), NULL);
}

/* Cancel current action */
static void
dev_cancel (FpDevice *device)
{
  FpiDeviceVfs0097 *self = FPI_DEVICE_VFS0097 (device);

  G_DEBUG_HERE ();

}

static gsize
read_dmi (const char *filename, char *buffer, int buffer_len)
{
  FILE *file;
  size_t read;

  if (!(file = fopen (filename, "r")))
    {
      g_warning ("Could not read %s", filename);
      buffer[0] = 0;
      return 0;
    }

  fgets (buffer, buffer_len, file);

  read = strlen (buffer);
  g_assert (read > 0);
  read--;

  // Remove newline
  buffer[read] = 0;
  return read;
}

static void
fpi_device_vfs0097_init (FpiDeviceVfs0097 *self)
{
  gchar seed[] = "VirtualBox\0" "0";

  self->seed_length = G_N_ELEMENTS (seed);
  self->seed = g_malloc0 (G_N_ELEMENTS (seed));
  memcpy (self->seed, seed, G_N_ELEMENTS (seed));

// TODO: Device is initialized via VirtualBox, so real HW id is not useful for now

//  char name[1024], serial[1024];
//  gsize name_len, serial_len;
//
//  name_len = read_dmi ("/sys/class/dmi/id/product_name", name, sizeof (name));
//  serial_len = read_dmi ("/sys/class/dmi/id/product_serial", serial, sizeof (serial));
//
//  if (name_len == 0)
//    {
//      // Set system id to default value (i.e. "VirtualBox")
//    }
//
//  self->seed = g_malloc0 (name_len + serial_len + 2);
//
//  memcpy (self->seed, name, name_len + 1);
//  memcpy (self->seed + name_len + 1, serial, serial_len + 1);

  g_debug ("Initialized seed value: %s", self->seed);
}

static void
fpi_device_vfs0097_class_init (FpiDeviceVfs0097Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = "vfs0097";
  dev_class->full_name = "Validity VFS0097";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = id_table;

  dev_class->open = dev_open;
  dev_class->close = dev_close;
  dev_class->enroll = dev_enroll;
  dev_class->delete = dev_delete;
  dev_class->identify = dev_identify;
  dev_class->verify = dev_verify;
  dev_class->cancel = dev_cancel;
  dev_class->list = dev_list;
}
