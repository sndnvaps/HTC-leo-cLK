/*
 * Copyright (c) 2008-2009, Google Inc. All rights reserved.
 * Copyright (c) 2009-2010, Code Aurora Forum. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <string.h>
#include <stdlib.h>
#include <debug.h>
#include <target/hsusb.h>
#include <target/clock.h>
#include <platform/iomap.h>
#include <platform/irqs.h>
#include <platform/interrupts.h>
#include <platform/timer.h>
#include <kernel/thread.h>
#include <reg.h>
#include <pcom.h>
#include <dev/udc.h>
#include <target/board_htcleo.h>

/* common code - factor out into a shared file */

struct udc_descriptor {
	struct udc_descriptor *next;
	unsigned short tag;	/* ((TYPE << 8) | NUM) */
	unsigned short len;	/* total length */
	unsigned char data[0];
};

struct udc_descriptor *udc_descriptor_alloc(unsigned type, unsigned num, unsigned len)
{
	struct udc_descriptor *desc;
	if ((len > 255) || (len < 2) || (num > 255) || (type > 255))
		return 0;

	if (!(desc = malloc(sizeof(struct udc_descriptor) + len)))
		return 0;

	desc->next = 0;
	desc->tag = (type << 8) | num;
	desc->len = len;
	desc->data[0] = len;
	desc->data[1] = type;

	return desc;
}

static struct udc_descriptor *desc_list = 0;
static unsigned next_string_id = 1;

void udc_descriptor_register(struct udc_descriptor *desc)
{
	desc->next = desc_list;
	desc_list = desc;
}

unsigned udc_string_desc_alloc(const char *str)
{
	unsigned len;
	struct udc_descriptor *desc;
	unsigned char *data;

	if (next_string_id > 255)
		return 0;

	if (!str)
		return 0;

	len = strlen(str);
	desc = udc_descriptor_alloc(TYPE_STRING, next_string_id, len * 2 + 2);
	if (!desc)
		return 0;
	next_string_id++;

	/* expand ascii string to utf16 */
	data = desc->data + 2;
	while (len-- > 0) {
		*data++ = *str++;
		*data++ = 0;
	}

	udc_descriptor_register(desc);
	return desc->tag & 0xff;
}

/* end of common code */

struct usb_request {
	struct udc_request req;
	struct ept_queue_item *item;
};

struct udc_endpoint {
	struct udc_endpoint *next;
	unsigned bit;
	struct ept_queue_head *head;
	struct usb_request *req;
	unsigned char num;
	unsigned char in;
	unsigned short maxpkt;
};

struct udc_endpoint *ept_list = 0;
struct ept_queue_head *epts = 0;

static int usb_online = 0;
static int usb_highspeed = 0;

static struct udc_device *the_device;
static struct udc_gadget *the_gadget;

static struct msm_hsusb_pdata *pdata = NULL;

/* check if USB cable is connected
 *
 * RETURN: If cable connected return 1
 * If cable disconnected return 0
 */
int is_usb_cable_connected(void)
{
	/*Verify B Session Valid Bit to verify vbus status */
	if (B_SESSION_VALID & readl(USB_OTGSC)) {
		return 1;
	} else {
		return 0;
	}
}

bool is_usb_connected(void) {
	if (pdata) {
		if (pdata->power_supply) {
			if (pdata->power_supply->is_usb_online)
				return pdata->power_supply->is_usb_online();
		}
	}
	return is_usb_cable_connected();
}

bool is_ac_connected(void) {
	if (pdata) {
		if (pdata->power_supply) {
			if (pdata->power_supply->is_ac_online)
				return pdata->power_supply->is_ac_online();
		}
	}
	return false;
}

int usb_charger_state(void) {
	if (pdata) {
		if (pdata->power_supply) {
			if (pdata->power_supply->charger_state)
				return pdata->power_supply->charger_state();
		}
	}
	return CHG_OFF;
}

static inline void set_charger_state(enum psy_charger_state state) {
	if (!pdata)
		return;
	
	if (!pdata->power_supply)
		return;

	if (pdata->power_supply->set_charger_state)
		pdata->power_supply->set_charger_state(state);
}

static inline bool psy_wants_charging(void) {
	if (pdata) {
		if (pdata->power_supply) {
			if (pdata->power_supply->want_charging)
				return pdata->power_supply->want_charging();
		}
	}
	return true;
}

static void ulpi_set_power(bool enabled) {
	if (!pdata)
		return;

	if (!pdata->set_ulpi_state)
		return;

	pdata->set_ulpi_state(enabled);
}

struct udc_endpoint *_udc_endpoint_alloc(unsigned num, unsigned in, unsigned max_pkt)
{
	struct udc_endpoint *ept;
	unsigned cfg;

	ept = malloc(sizeof(*ept));

	ept->maxpkt = max_pkt;
	ept->num = num;
	ept->in = ! !in;
	ept->req = 0;

	cfg = CONFIG_MAX_PKT(max_pkt) | CONFIG_ZLT;

	if (ept->in) {
		ept->bit = EPT_TX(ept->num);
	} else {
		ept->bit = EPT_RX(ept->num);
		if (num == 0)
			cfg |= CONFIG_IOS;
	}

	ept->head = epts + (num * 2) + (ept->in);
	ept->head->config = cfg;

	ept->next = ept_list;
	ept_list = ept;

	arch_clean_invalidate_cache_range((addr_t)ept->head, 64);

	return ept;
}

static unsigned ept_alloc_table = EPT_TX(0) | EPT_RX(0);

struct udc_endpoint *udc_endpoint_alloc(unsigned type, unsigned maxpkt)
{
	struct udc_endpoint *ept;
	unsigned n;
	unsigned in;

	if (type == UDC_TYPE_BULK_IN) {
		in = 1;
	} else if (type == UDC_TYPE_BULK_OUT) {
		in = 0;
	} else {
		return 0;
	}

	for (n = 1; n < 16; n++) {
		unsigned bit = in ? EPT_TX(n) : EPT_RX(n);
		if (ept_alloc_table & bit)
			continue;
		ept = _udc_endpoint_alloc(n, in, maxpkt);
		if (ept)
			ept_alloc_table |= bit;
		return ept;
	}
	return 0;
}

void udc_endpoint_free(struct udc_endpoint *ept)
{
	/* todo */
}

static void endpoint_enable(struct udc_endpoint *ept, unsigned yes)
{
	unsigned n = readl(USB_ENDPTCTRL(ept->num));

	if (yes) {
		if (ept->in) {
			n |= (CTRL_TXE | CTRL_TXR | CTRL_TXT_BULK);
		} else {
			n |= (CTRL_RXE | CTRL_RXR | CTRL_RXT_BULK);
		}

		if (ept->num != 0) {
			/* XXX should be more dynamic... */
			if (usb_highspeed) {
				ept->head->config = CONFIG_MAX_PKT(512) | CONFIG_ZLT;
			} else {
				ept->head->config = CONFIG_MAX_PKT(64) | CONFIG_ZLT;
			}
		}
	}
	writel(n, USB_ENDPTCTRL(ept->num));
}

struct udc_request *udc_request_alloc(void)
{
	struct usb_request *req;
	req = malloc(sizeof(*req));
	req->req.buf = 0;
	req->req.length = 0;
	req->item = memalign(32, 32);
	return &req->req;
}

void udc_request_free(struct udc_request *req)
{
	free(req);
}

int udc_request_queue(struct udc_endpoint *ept, struct udc_request *_req)
{
	struct usb_request *req = (struct usb_request *)_req;
	struct ept_queue_item *item = req->item;
	unsigned phys = (unsigned)req->req.buf;

	item->next = TERMINATE;
	item->info = INFO_BYTES(req->req.length) | INFO_IOC | INFO_ACTIVE;
	item->page0 = phys;
	item->page1 = (phys & 0xfffff000) + 0x1000;
	item->page2 = (phys & 0xfffff000) + 0x2000;
	item->page3 = (phys & 0xfffff000) + 0x3000;
	item->page4 = (phys & 0xfffff000) + 0x4000;

	enter_critical_section();
	ept->head->next = (unsigned)item;
	ept->head->info = 0;
	ept->req = req;

	arch_clean_invalidate_cache_range((addr_t) ept, sizeof(struct udc_endpoint));
	arch_clean_invalidate_cache_range((addr_t) ept->head, sizeof(struct ept_queue_head));
	arch_clean_invalidate_cache_range((addr_t) ept->req, sizeof(struct usb_request));
	arch_clean_invalidate_cache_range((addr_t) req->req.buf, req->req.length);
	arch_clean_invalidate_cache_range((addr_t) ept->req->item, sizeof(struct ept_queue_item));

	writel(ept->bit, USB_ENDPTPRIME);
	exit_critical_section();
	return 0;
}

static void handle_ept_complete(struct udc_endpoint *ept)
{
	struct ept_queue_item *item;
	unsigned actual;
	int status;
	struct usb_request *req;

	arch_clean_invalidate_cache_range((addr_t) ept, sizeof(struct udc_endpoint));
	arch_clean_invalidate_cache_range((addr_t) ept->req, sizeof(struct usb_request));

	req = ept->req;
	if (req) {
		ept->req = 0;

		item = req->item;

		/* For some reason we are getting the notification for
		 * transfer completion before the active bit has cleared.
		 * HACK: wait for the ACTIVE bit to clear:
		 */
		do
		{
			arch_clean_invalidate_cache_range((addr_t) item, sizeof(struct ept_queue_item));
		} while (readl(&(item->info)) & INFO_ACTIVE);

		arch_clean_invalidate_cache_range((addr_t) req->req.buf, req->req.length);

		if (item->info & 0xff) {
			actual = 0;
			status = -1;
		} else {
			actual = req->req.length - ((item->info >> 16) & 0x7fff);
			status = 0;
		}
		if (req->req.complete)
			req->req.complete(&req->req, actual, status);
	}
}

static const char *reqname(unsigned r)
{
	switch (r) {
		case GET_STATUS:         return "GET_STATUS";
		case CLEAR_FEATURE:      return "CLEAR_FEATURE";
		case SET_FEATURE:        return "SET_FEATURE";
		case SET_ADDRESS:        return "SET_ADDRESS";
		case GET_DESCRIPTOR:     return "GET_DESCRIPTOR";
		case SET_DESCRIPTOR:     return "SET_DESCRIPTOR";
		case GET_CONFIGURATION:  return "GET_CONFIGURATION";
		case SET_CONFIGURATION:  return "SET_CONFIGURATION";
		case GET_INTERFACE:      return "GET_INTERFACE";
		case SET_INTERFACE:      return "SET_INTERFACE";
		default:                 return "*UNKNOWN*";
	}
}

static struct udc_endpoint *ep0in, *ep0out;
static struct udc_request *ep0req;

static void setup_ack(void)
{
	ep0req->complete = 0;
	ep0req->length = 0;
	udc_request_queue(ep0in, ep0req);
}

static void ep0in_complete(struct udc_request *req, unsigned actual, int status)
{
	if (status == 0) {
		req->length = 0;
		req->complete = 0;
		udc_request_queue(ep0out, req);
	}
}

static void setup_tx(void *buf, unsigned len)
{
	memcpy(ep0req->buf, buf, len);
	ep0req->complete = ep0in_complete;
	ep0req->length = len;
	udc_request_queue(ep0in, ep0req);
}

static unsigned char usb_config_value = 0;

#define SETUP(type,request) (((type) << 8) | (request))

static void handle_setup(struct udc_endpoint *ept)
{
	struct setup_packet s;

	arch_clean_invalidate_cache_range((addr_t) ept->head->setup_data, sizeof(struct ept_queue_head));
	memcpy(&s, ept->head->setup_data, sizeof(s));
	writel(ept->bit, USB_ENDPTSETUPSTAT);

	switch (SETUP(s.type, s.request)) {
		case SETUP(DEVICE_READ, GET_STATUS):
		{
			unsigned zero = 0;
			if (s.length == 2) {
				setup_tx(&zero, 2);
				return;
			}
		}break;
		case SETUP(DEVICE_READ, GET_DESCRIPTOR):
		{
			struct udc_descriptor *desc;
			/* usb_highspeed? */
			for (desc = desc_list; desc; desc = desc->next) {
				if (desc->tag == s.value) {
					unsigned len = desc->len;
					if (len > s.length) len = s.length;
					setup_tx(desc->data, len);
					return;
				}
			}
		}break;
		case SETUP(DEVICE_READ, GET_CONFIGURATION):
		{
			/* disabling this causes data transaction failures on OSX. Why? */
			if ((s.value == 0) && (s.index == 0) && (s.length == 1)) {
				setup_tx(&usb_config_value, 1); return;
			}
		}break;
		case SETUP(DEVICE_WRITE, SET_CONFIGURATION):
		{
			if (s.value == 1) {
				struct udc_endpoint *ept;
				/* enable endpoints */
				for (ept = ept_list; ept; ept = ept->next) {
					if (ept->num == 0)
						continue;
					endpoint_enable(ept, s.value);
				}
				usb_config_value = 1;
				if (is_usb_connected())
					set_charger_state(CHG_USB_HIGH);
				the_gadget->notify(the_gadget, UDC_EVENT_ONLINE);
			} else {
				writel(0, USB_ENDPTCTRL(1));
				usb_config_value = 0;
				the_gadget->notify(the_gadget, UDC_EVENT_OFFLINE);
			}
			setup_ack();
			usb_online = s.value ? 1 : 0;
			//usb_status(s.value ? 1 : 0, usb_highspeed);
			return;
		}break;
		case SETUP(DEVICE_WRITE, SET_ADDRESS):
		{
			/* write address delayed (will take effect
			 ** after the next IN txn)
			 */
			writel((s.value << 25) | (1 << 24), USB_DEVICEADDR);
			setup_ack();
			return;
		}break;
		case SETUP(INTERFACE_WRITE, SET_INTERFACE):
		{
			/* if we ack this everything hangs */
			/* per spec, STALL is valid if there is not alt func */
			goto stall;
		}break;
		case SETUP(ENDPOINT_WRITE, CLEAR_FEATURE):
		{
			struct udc_endpoint *ept;
			unsigned num = s.index & 15;
			unsigned in = ! !(s.index & 0x80);

			if ((s.value == 0) && (s.length == 0)) {
				//DBG("clr feat %d %d\n", num, in);
				for (ept = ept_list; ept; ept = ept->next) {
					if ((ept->num == num) && (ept->in == in)) {
						endpoint_enable(ept, 1);
						setup_ack();
						return;
					}
				}
			}
		}break;
	}

stall:
	writel((1 << 16) | (1 << 0), USB_ENDPTCTRL(ept->num));
}

unsigned ulpi_read(unsigned reg)
{
	unsigned timeout = 10000;

	/* initiate read operation */
	writel(ULPI_RUN | ULPI_READ | ULPI_ADDR(reg), USB_ULPI_VIEWPORT);

	/* wait for completion */
	while ((readl(USB_ULPI_VIEWPORT) & ULPI_RUN) && (--timeout));
	
	if (!timeout) {
		return -1UL;
	}

	return ULPI_DATA_READ(readl(USB_ULPI_VIEWPORT));
}

void ulpi_write(unsigned val, unsigned reg)
{
	unsigned timeout = 10000;
	
	/* initiate write operation */
	writel(ULPI_RUN | ULPI_WRITE | ULPI_ADDR(reg) | ULPI_DATA(val), USB_ULPI_VIEWPORT);

	/* wait for completion */
	while ((readl(USB_ULPI_VIEWPORT) & ULPI_RUN) && (--timeout));
}

/*
#define CLKRGM_APPS_RESET_USBH      37
#define CLKRGM_APPS_RESET_USB_PHY   34
static void msm_hsusb_apps_reset_link(int reset)
{
	int ret;
	unsigned usb_id = CLKRGM_APPS_RESET_USBH;

	if (reset)
		ret = msm_proc_comm(PCOM_CLK_REGIME_SEC_RESET_ASSERT, &usb_id, NULL);
	else
		ret = msm_proc_comm(PCOM_CLK_REGIME_SEC_RESET_DEASSERT, &usb_id, NULL);
				
	if (ret)
		printf("%s: Cannot set reset to %d (%d)\n", __func__, reset, ret);
}

static void msm_hsusb_apps_reset_phy(void)
{
	int ret;
	unsigned usb_phy_id = CLKRGM_APPS_RESET_USB_PHY;

	ret = msm_proc_comm(PCOM_CLK_REGIME_SEC_RESET_ASSERT, &usb_phy_id, NULL);
	if (ret) {
		printf("%s: Cannot assert (%d)\n", __func__, ret);
		return;
	}
	
	mdelay(1);
	
	ret = msm_proc_comm(PCOM_CLK_REGIME_SEC_RESET_DEASSERT, &usb_phy_id, NULL);
	if (ret) {
		printf("%s: Cannot assert (%d)\n", __func__, ret);
		return;
	}
}

#define ULPI_VERIFY_MAX_LOOP_COUNT  3
static int msm_hsusb_phy_verify_access(void)
{
	int temp;

	for (temp = 0; temp < ULPI_VERIFY_MAX_LOOP_COUNT; temp++) {
		if (ulpi_read(ULPI_DEBUG) != (unsigned)-1)
			break;
		msm_hsusb_apps_reset_phy();
	}

	if (temp == ULPI_VERIFY_MAX_LOOP_COUNT) {
		printf("%s: ulpi read failed for %d times\n", __func__, ULPI_VERIFY_MAX_LOOP_COUNT);
		return -1;
	}

	return 0;
}

static unsigned msm_hsusb_ulpi_read_with_reset(unsigned reg)
{
	int temp;
	unsigned res;

	for (temp = 0; temp < ULPI_VERIFY_MAX_LOOP_COUNT; temp++) {
		res = ulpi_read(reg);
		if (res != -1)
			return res;
		msm_hsusb_apps_reset_phy();
	}

	printf("%s: ulpi read failed for %d times\n", __func__, ULPI_VERIFY_MAX_LOOP_COUNT);
	return -1;
}

static int msm_hsusb_ulpi_write_with_reset(unsigned val, unsigned reg)
{
	int temp;
	int res;

	for (temp = 0; temp < ULPI_VERIFY_MAX_LOOP_COUNT; temp++) {
		res = ulpi_write(val, reg);
		if (!res)
			return 0;
		msm_hsusb_apps_reset_phy();
	}

	printf("%s: ulpi write failed for %d times\n", __func__, ULPI_VERIFY_MAX_LOOP_COUNT);
	return -1;
}

static int msm_hsusb_phy_caliberate(void)
{
	int ret;
	unsigned res;

	ret = msm_hsusb_phy_verify_access();
	if (ret)
		return -ETIMEDOUT;

	res = msm_hsusb_ulpi_read_with_reset(ULPI_FUNC_CTRL_CLR);
	if (res == -1)
		return -ETIMEDOUT;

	res = msm_hsusb_ulpi_write_with_reset(res | ULPI_SUSPENDM, ULPI_FUNC_CTRL_CLR);
	if (res)
		return -ETIMEDOUT;

	msm_hsusb_apps_reset_phy();

	return msm_hsusb_phy_verify_access();
}

#define USB_LINK_RESET_TIMEOUT      (msecs_to_jiffies(10))
void msm_hsusb_8x50_phy_reset(void)
{
	u32 temp;
	unsigned long timeout;
	printf("msm_hsusb_phy_reset\n");

	msm_hsusb_apps_reset_link(1);
	msm_hsusb_apps_reset_phy();
	msm_hsusb_apps_reset_link(0);

	// select ULPI phy
	temp = (readl(USB_PORTSC) & ~PORTSC_PTS);
	writel(temp | PORTSC_PTS_ULPI, USB_PORTSC);

	if (msm_hsusb_phy_caliberate()) {
		usb_phy_error = 1;
		return;
	}

	// soft reset phy
	writel(USBCMD_RESET, USB_USBCMD);
	while (readl(USB_USBCMD) & USBCMD_RESET)
		msleep(1);

	usb_phy_error = 0;

	return;
}
*/

static inline void msm_otg_xceiv_reset()
{
	clk_disable(USB_HS_CLK);
	clk_disable(USB_HS_PCLK);
	thread_sleep(20);
	clk_enable(USB_HS_PCLK);
	clk_enable(USB_HS_CLK);
	thread_sleep(20);
}

void msm_hsusb_init(struct msm_hsusb_pdata *new_pdata) {
	pdata = new_pdata;
}

static inline void disable_pullup(void) {
	writel(0x80000, USB_USBCMD);
	thread_sleep(10);
}

static inline void enable_pullup(void) {
	writel(0x80001, USB_USBCMD);
	
}

static void udc_reset(void) {
	msm_otg_xceiv_reset();
	ulpi_set_power(true);
	thread_sleep(100);
	msm_otg_xceiv_reset();
	
	/* disable usb interrupts and otg */
	writel(0, USB_OTGSC);
	thread_sleep(5);
	
	/* select ULPI phy */
	writel(0x81000000, USB_PORTSC);
	
	/* RESET */
	writel(0x80002, USB_USBCMD);
	thread_sleep(20);

	writel((unsigned)epts, USB_ENDPOINTLISTADDR);
	
	/* select DEVICE mode */
	writel(0x02, USB_USBMODE);
	thread_sleep(1);
	
	writel(0xffffffff, USB_ENDPTFLUSH);
	thread_sleep(20);
}

int udc_init(struct udc_device *dev) {
	epts = memalign(4096, 4096);

	memset(epts, 0, 32 * sizeof(struct ept_queue_head));
	
	udc_reset();
	//dprintf(SPEW, "USB ID %08x\n", readl(USB_ID));

	ep0out = _udc_endpoint_alloc(0, 0, 64);
	ep0in = _udc_endpoint_alloc(0, 1, 64);
	ep0req = udc_request_alloc();
	ep0req->buf = malloc(4096);

	{
		/* create and register a language table descriptor */
		/* language 0x0409 is US English */
		struct udc_descriptor *desc = udc_descriptor_alloc(TYPE_STRING, 0, 4);
		desc->data[2] = 0x09;
		desc->data[3] = 0x04;
		udc_descriptor_register(desc);
	}
	
	the_device = dev;
	return 0;
}

enum handler_return udc_interrupt(void *arg)
{
	struct udc_endpoint *ept;
	unsigned ret = INT_NO_RESCHEDULE;
	unsigned n = readl(USB_USBSTS);
	writel(n, USB_USBSTS);

	n &= (STS_SLI | STS_URI | STS_PCI | STS_UI | STS_UEI);

	if (n == 0)
		return ret;

	if (n & STS_URI) {
		writel(readl(USB_ENDPTCOMPLETE), USB_ENDPTCOMPLETE);
		writel(readl(USB_ENDPTSETUPSTAT), USB_ENDPTSETUPSTAT);
		writel(0xffffffff, USB_ENDPTFLUSH);
		writel(0, USB_ENDPTCTRL(1));
		usb_online = 0;
		usb_config_value = 0;
		the_gadget->notify(the_gadget, UDC_EVENT_OFFLINE);

		/* error out any pending reqs */
		for (ept = ept_list; ept; ept = ept->next) {
			/* ensure that ept_complete considers
			 * this to be an error state
			 */
			if (ept->req) {
				ept->req->item->info = INFO_HALTED;
				handle_ept_complete(ept);
			}
		}
		//usb_status(0, usb_highspeed);
	}
	if (n & STS_SLI) {
		if (is_usb_connected())
			set_charger_state(CHG_USB_HIGH);
	}
	if (n & STS_PCI) {
		unsigned spd = (readl(USB_PORTSC) >> 26) & 3;
		if (spd == 2) {
			usb_highspeed = 1;
		} else {
			usb_highspeed = 0;
		}
		if (is_usb_connected()) {
			set_charger_state(usb_config_value ? CHG_USB_HIGH : CHG_USB_LOW);
		}
	}
	if ((n & STS_UI) || (n & STS_UEI)) {
		n = readl(USB_ENDPTSETUPSTAT);
		if (n & EPT_RX(0)) {
			handle_setup(ep0out);
			ret = INT_RESCHEDULE;
		}

		n = readl(USB_ENDPTCOMPLETE);
		if (n != 0) {
			writel(n, USB_ENDPTCOMPLETE);
		}

		for (ept = ept_list; ept; ept = ept->next) {
			if (n & ept->bit) {
				handle_ept_complete(ept);
				ret = INT_RESCHEDULE;
			}
		}
	}
	return ret;
}

int udc_register_gadget(struct udc_gadget *gadget)
{
	if (the_gadget) return -1;

	the_gadget = gadget;
	return 0;
}

static void udc_ept_desc_fill(struct udc_endpoint *ept, unsigned char *data)
{
	data[0] = 7;
	data[1] = TYPE_ENDPOINT;
	data[2] = ept->num | (ept->in ? 0x80 : 0x00);
	data[3] = 0x02;		/* bulk -- the only kind we support */
	data[4] = ept->maxpkt;
	data[5] = ept->maxpkt >> 8;
	data[6] = ept->in ? 0x00 : 0x01;
}

static unsigned udc_ifc_desc_size(struct udc_gadget *g)
{
	return 9 + g->ifc_endpoints * 7;
}

static void udc_ifc_desc_fill(struct udc_gadget *g, unsigned char *data)
{
	unsigned n;

	data[0] = 0x09;
	data[1] = TYPE_INTERFACE;
	data[2] = 0x00;		/* ifc number */
	data[3] = 0x00;		/* alt number */
	data[4] = g->ifc_endpoints;
	data[5] = g->ifc_class;
	data[6] = g->ifc_subclass;
	data[7] = g->ifc_protocol;
	data[8] = udc_string_desc_alloc(g->ifc_string);

	data += 9;
	for (n = 0; n < g->ifc_endpoints; n++) {
		udc_ept_desc_fill(g->ept[n], data);
		data += 7;
	}
}

int udc_start(void)
{
	struct udc_descriptor *desc;
	unsigned char *data;
	unsigned size;

	if (!the_device) return -1;
	if (!the_gadget) return -1;

	/* create our device descriptor */
	desc = udc_descriptor_alloc(TYPE_DEVICE, 0, 18);
	data = desc->data;
	data[2] = 0x00;		/* usb spec minor rev */
	data[3] = 0x02;     /* usb spec major rev */
	data[4] = 0x00;     /* class */
	data[5] = 0x00;     /* subclass */
	data[6] = 0x00;     /* protocol */
	data[7] = 0x40;     /* max packet size on ept 0 */
	memcpy(data + 8, &the_device->vendor_id, sizeof(short));
	memcpy(data + 10, &the_device->product_id, sizeof(short));
	memcpy(data + 12, &the_device->version_id, sizeof(short));
	data[14] = udc_string_desc_alloc(the_device->manufacturer);
	data[15] = udc_string_desc_alloc(the_device->product);
	data[16] = udc_string_desc_alloc(the_device->serialno);
	data[17] = 1;		/* number of configurations */
	udc_descriptor_register(desc);

	/* create our configuration descriptor */
	size = 9 + udc_ifc_desc_size(the_gadget);
	desc = udc_descriptor_alloc(TYPE_CONFIGURATION, 0, size);
	data = desc->data;
	data[0] = 0x09;
	data[2] = size;
	data[3] = size >> 8;
	data[4] = 0x01;		/* number of interfaces */
	data[5] = 0x01;		/* configuration value */
	data[6] = 0x00;		/* configuration string */
	data[7] = 0x80;		/* attributes */
	data[8] = 0x80;		/* max power (250ma) -- todo fix this */
	udc_ifc_desc_fill(the_gadget, data + 9);
	udc_descriptor_register(desc);

	register_int_handler(INT_USB_HS, udc_interrupt, (void *)0);
	writel(STS_URI | STS_SLI | STS_UI | STS_PCI, USB_USBINTR);
	unmask_interrupt(INT_USB_HS);

	/* go to RUN mode (D+ pullup enable) */
	enable_pullup();

	return 0;
}

int udc_stop(void)
{
	writel(0, USB_USBINTR);
	mask_interrupt(INT_USB_HS);

	/* disable pullup */
	disable_pullup();

	return 0;
}

/* check for USB connection assuming USB is not pulled up.
 * It looks for suspend state bit in PORTSC register.
 *
 * RETURN: If cable connected return 1
 * If cable disconnected return 0
 */
int usb_cable_status(void)
{
	unsigned ret = 0;
	/*Verify B Session Valid Bit to verify vbus status */
	writel(0x00080001, USB_USBCMD);
	thread_sleep(100);

	/*Check reset value of suspend state bit */
	if (!((1 << 7) & readl(USB_PORTSC))) {
		ret = 1;
	}
	udc_stop();

	return ret;
}

/* Charger type detection code
 */
int usb_charger_detect_type(void)
{
    int ret = CHG_TYPE_UNDEFINED;

    if ((readl(USB_PORTSC) & PORTSC_LS) == PORTSC_LS)
		ret = CHG_TYPE_WALL;
    else
		ret = CHG_TYPE_PC_HOST;

    return ret;
}

void usb_charger_change_state(void)
{
	// First check usb cable status
	bool usb_cable_connected = is_usb_connected();
	// If not connected, set charger state to CHG_OFF
	// and just return
	if (!usb_cable_connected) {
		if (usb_charger_state() != CHG_OFF ) {							
			enable_pullup();
			set_charger_state(CHG_OFF);
		}
		return;
	}
	// Else if usb cable is connected
	// proceed according to battery voltage
	if (psy_wants_charging()) {
		// If battery needs charging, check charger type
		int charge_type = usb_charger_detect_type();
		
		if (charge_type == CHG_TYPE_PC_HOST) {
			// PC charger detected
			if (usb_charger_state() != CHG_USB_LOW ) {	
				enable_pullup();
				set_charger_state(CHG_USB_LOW);
			}
		} else if (charge_type == CHG_TYPE_WALL) {
			// Wall charger detected
			if (usb_charger_state() != CHG_AC ) {	
				disable_pullup();
				/*USB Spoof Disconnect Failure
				  Symptoms:
				  In USB peripheral mode, writing '0' to Run/Stop bit of the
				  USBCMD register doesn't cause USB disconnection (spoof disconnect).
				  The PC host doesn't detect the disconnection and the phone remains
				  active on Windows device manager.

				  Suggested Workaround:
				  After writing '0' to Run/Stop bit of USBCMD, also write 0x48 to ULPI
				  "Function Control" register. This can be done via the ULPI VIEWPORT
				  register (offset 0x170) by writing a value of 0x60040048.
				 */
				ulpi_write(0x48, 0x04);
				set_charger_state(CHG_AC);
			}
		} else {
			// Charger undefined
			if (usb_charger_state() != CHG_OFF ) {
				enable_pullup();
				set_charger_state(CHG_OFF);
			}
		}
	} else {
		// Battery is full, set charger state to CHG_OFF_FULL_BAT
		if (usb_charger_state() != CHG_OFF_FULL_BAT ) {									
			enable_pullup();
			set_charger_state(CHG_OFF_FULL_BAT);
		}
	}
	//thread_sleep(10);
}
