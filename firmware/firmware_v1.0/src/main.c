#include <zephyr/kernel.h>
#include "transport.h"
#include "mic.h"
#include "utils.h"
#include "led.h"
#include "config.h"
#include "audio.h"
#include "codec.h"
#include "usb_power.h"
#include "lib/battery/battery.h"
#include <zephyr/sys/reboot.h>

// USB 給電中は録音しない（もみじ庵改修）。
// 起動時に USB 給電があれば充電専用モード（マイク電源 Low・Bluetooth を起動しない・緑 LED）に入る。
// 録音中に USB 給電を検知したら即マイクを止めて再起動し、充電専用モードで立ち上がり直す。
// 充電専用モードで USB が抜けたら再起動し、通常の起動（＝録音）に戻る。
// スライドスイッチは電池を切るだけで USB 給電の経路は切れないため、スイッチ OFF でも USB を挿すと
// 基板には電源が入る。この改修でその状態でもマイクと Bluetooth は動かない。
#define POWER_POLL_MS 100
#define LED_UPDATE_EVERY_POLLS 5      // 従来どおり 500ms ごとに LED を更新
#define USB_REMOVED_DEBOUNCE_POLLS 5  // 抜けた判定は 500ms 連続で給電なし（挿し込み時のチャタリング対策）

static void codec_handler(uint8_t *data, size_t len)
{
	broadcast_audio_packets(data, len); // Errors are logged inside
}

static void mic_handler(int16_t *buffer)
{
	codec_receive_pcm(buffer, MIC_BUFFER_SAMPLES); // Errors are logged inside
}

void bt_ctlr_assert_handle(char *name, int type)
{
	if (name != NULL)
	{
		printk("Bt assert-> %s", name);
	}
}

bool is_connected = false;
bool is_charging = false;

void set_led_state()
{
	// Recording and connected state - BLUE
	if (is_connected)
	{
		set_led_red(false);
		set_led_green(false);
		set_led_blue(true);
		return;
	}

	// Recording but lost connection - RED
	if (!is_connected)
	{
		set_led_red(true);
		set_led_green(false);
		set_led_blue(false);
		return;
	}

	// Not recording, but charging - WHITE
	if (is_charging)
	{
		set_led_red(true);
		set_led_green(true);
		set_led_blue(true);
		return;
	}

	// Not recording - OFF
	set_led_red(false);
	set_led_green(false);
	set_led_blue(false);
}

static void charge_only_mode(void)
{
	printk("USB power detected: charge-only mode (mic off, Bluetooth off)\n");

	// マイクは起動しないが、電源ピンを明示的に Low にしておく
	mic_power_off();

	set_led_red(false);
	set_led_blue(false);
	set_led_green(true);

	// 充電電流の設定は通常モード（transport_start 内）と同じにそろえる
	int battErr = 0;
	battErr |= battery_init();
	battErr |= battery_charge_start();
	if (battErr)
	{
		printk("Battery init failed (err %d)\n", battErr);
	}

	int absent_polls = 0;
	while (1)
	{
		if (usb_power_present())
		{
			absent_polls = 0;
		}
		else if (++absent_polls >= USB_REMOVED_DEBOUNCE_POLLS)
		{
			printk("USB power removed: rebooting to normal mode\n");
			sys_reboot(SYS_REBOOT_COLD);
		}
		k_msleep(POWER_POLL_MS);
	}
}

// Main loop
int main(void)
{
	// Led start
	ASSERT_OK(led_start());

	// USB 給電中はマイクも Bluetooth も起動しない（戻らない）
	if (usb_power_present())
	{
		charge_only_mode();
	}

	set_led_blue(true);

	// Transport start
	ASSERT_OK(transport_start());

	// Codec start
	set_codec_callback(codec_handler);
	ASSERT_OK(codec_start());

	// Mic start
	set_mic_callback(mic_handler);
	ASSERT_OK(mic_start());

	int polls = 0;
	while (1)
	{
		// 録音中に USB 給電が来たら、即マイクを止めて再起動（充電専用モードへ）
		if (usb_power_present())
		{
			mic_power_off();
			printk("USB power detected while recording: rebooting to charge-only mode\n");
			sys_reboot(SYS_REBOOT_COLD);
		}

		if (++polls >= LED_UPDATE_EVERY_POLLS)
		{
			polls = 0;
			set_led_state();
		}
		k_msleep(POWER_POLL_MS);
	}

	// Unreachable
	return 0;
}
