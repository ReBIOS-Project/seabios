#ifndef __REBIOS_USB_H
#define __REBIOS_USB_H

struct disk_op_s;
int rebios_usb_process_op(struct disk_op_s *op);
void rebios_usb_setup(void);

#endif // rebios-usb.h
