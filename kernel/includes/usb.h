/* =============================================================================
 * usb.h — USB common constants, descriptor layouts, boot-keyboard HID tables.
 *
 * Shared between the host-controller driver (xhci.c) and the class
 * drivers (usb_hid.c).  Wire-format constants follow the USB 2.0 and
 * HID 1.11 specs verbatim; structs use `__attribute__((packed))` so
 * direct memcpy from DMA buffers works.
 *
 * Scope: just what M15 needs.  We're shipping a single class driver
 * (boot-mode keyboard), so e.g. isochronous transfer types and full
 * HID report-descriptor parsing are intentionally absent.
 * ============================================================================= */

#ifndef USB_H
#define USB_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * bmRequestType (USB 2.0 §9.3, table 9-2).
 *
 *   bit 7    : data direction (0 = host->device, 1 = device->host)
 *   bits 6-5 : type            (0 = standard, 1 = class, 2 = vendor)
 *   bits 4-0 : recipient       (0 = device, 1 = interface, 2 = endpoint)
 * --------------------------------------------------------------------------- */
#define USB_DIR_OUT          0x00
#define USB_DIR_IN           0x80
#define USB_TYPE_STANDARD    (0u << 5)
#define USB_TYPE_CLASS       (1u << 5)
#define USB_RECIP_DEVICE     0
#define USB_RECIP_INTERFACE  1
#define USB_RECIP_ENDPOINT   2

/* Standard request codes (USB 2.0 table 9-4). */
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_SET_CONFIG      0x09
#define USB_REQ_GET_CONFIG      0x08

/* HID class requests (HID 1.11 §7.2). */
#define HID_REQ_GET_REPORT      0x01
#define HID_REQ_SET_REPORT      0x09
#define HID_REQ_GET_PROTOCOL    0x03
#define HID_REQ_SET_PROTOCOL    0x0B

/* Descriptor types — high byte of wValue in GET_DESCRIPTOR. */
#define USB_DT_DEVICE          0x01
#define USB_DT_CONFIG          0x02
#define USB_DT_STRING          0x03
#define USB_DT_INTERFACE       0x04
#define USB_DT_ENDPOINT        0x05
#define USB_DT_HID             0x21
#define USB_DT_HID_REPORT      0x22

/* USB class codes (USB-IF defined). */
#define USB_CLASS_HID          0x03
/* HID subclass / protocol for the boot interface. */
#define HID_SUBCLASS_BOOT      0x01
#define HID_PROTO_KEYBOARD     0x01

/* HID protocol setting passed to SET_PROTOCOL. */
#define HID_PROTOCOL_BOOT      0
#define HID_PROTOCOL_REPORT    1

/* ---------------------------------------------------------------------------
 * Descriptor layouts.  All multi-byte fields are little-endian as on
 * the wire.  We always read whole descriptors via DMA, then cast.
 * --------------------------------------------------------------------------- */

struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;          /* total of this + every following */
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;      /* bit 7 = direction (1=IN), bits 3-0 = number */
    uint8_t  bmAttributes;          /* low 2 bits = type (0=ctrl, 1=iso, 2=bulk, 3=intr) */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

#define USB_EP_TYPE_CONTROL    0
#define USB_EP_TYPE_ISOCHR     1
#define USB_EP_TYPE_BULK       2
#define USB_EP_TYPE_INTERRUPT  3

/* Setup packet (USB 2.0 §9.3) — always 8 bytes, host-to-device on EP0. */
struct usb_setup_packet {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

/* ---------------------------------------------------------------------------
 * Boot keyboard report (HID 1.11 §B.1) — exactly 8 bytes.
 *
 *   byte 0   : modifier mask
 *               bit 0 = LCtrl   bit 4 = RCtrl
 *               bit 1 = LShift  bit 5 = RShift
 *               bit 2 = LAlt    bit 6 = RAlt
 *               bit 3 = LGUI    bit 7 = RGUI
 *   byte 1   : reserved (OEM)
 *   bytes 2-7: up to 6 currently-active HID usage IDs (0 = empty slot,
 *              1 = ErrorRollOver, 2 = POSTFail, 3 = ErrorUndefined).
 *
 * Boot-mode keyboards always emit this fixed layout regardless of the
 * device's full report descriptor — that's the whole point of the
 * "boot interface" subclass.
 * --------------------------------------------------------------------------- */
struct hid_boot_kbd_report {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
} __attribute__((packed));

#define HID_MOD_LCTRL   0x01
#define HID_MOD_LSHIFT  0x02
#define HID_MOD_LALT    0x04
#define HID_MOD_LGUI    0x08
#define HID_MOD_RCTRL   0x10
#define HID_MOD_RSHIFT  0x20
#define HID_MOD_RALT    0x40
#define HID_MOD_RGUI    0x80

/* HID class driver entry — called from xhci.c when a HID boot keyboard
 * is detected on an endpoint.  Hands every fresh boot-report to the
 * class driver, which diffs it against the previous report, decodes
 * fresh key presses to ASCII, and pushes into vc_kbd_push. */
void usb_hid_kbd_handle_report(const uint8_t* report);

/* xHCI driver poll hook.  Called periodically (e.g. from the PIT
 * tick) to drain the Event Ring — we don't have MSI/MSI-X wired up
 * yet, so polling keeps HID reports flowing without needing IRQs.
 * No-op until the controller is fully initialized. */
void xhci_poll(void);

#endif
