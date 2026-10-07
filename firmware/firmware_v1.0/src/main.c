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

// 起動のしかたで録音するかを決める（もみじ庵改修）。
// - 電池で起動（スイッチ ON・USB なし）: 録音。あとから USB を挿しても録音を続けたまま充電する（勤務中）
// - USB で起動（スイッチ OFF のまま USB を挿した）: 録音しない充電専用モード（マイク電源 Low・Bluetooth を起動しない・赤 LED）。
//   そのあとスイッチを ON にすると録音しないまま充電される（寝る前）
// - 充電専用モードで USB が抜けたら再起動。電池がつながっていれば（スイッチ ON）録音で立ち上がる（朝）
// スライドスイッチは電池を切るだけで USB 給電の経路は切れないため、スイッチ OFF でも USB を挿すと基板に電源が入る。
#define POWER_POLL_MS 100
#define USB_REMOVED_DEBOUNCE_MS 500   // 抜けた判定は 500ms 以上連続で給電なし（挿し込み時のチャタリング対策）
#define BOOT_USB_WATCH_MS 2000        // 起動直後この間に一度でも USB 給電を見たら録音しない（立ち上がりの遅い電源・接点のバタつき対策）
#define BOOT_USB_WATCH_GAP_MS 10

// 起動直後 BOOT_USB_WATCH_MS の間に一度でも USB 給電を見たら true（録音しない側に倒す）。
// 誤って true になっても充電専用モードの「500ms 給電なし → 再起動」で録音に戻る。
static bool usb_power_seen_at_boot(void)
{
	for (int elapsed = 0; elapsed < BOOT_USB_WATCH_MS; elapsed += BOOT_USB_WATCH_GAP_MS)
	{
		if (usb_power_present())
		{
			return true;
		}
		k_msleep(BOOT_USB_WATCH_GAP_MS);
	}
	return false;
}

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
static bool led_blink_phase = false;

// 録音中の LED（もみじ庵改修）: 青=録音中 / 緑=録音しながら充電 / スマホ未接続ならその色で点滅。
// 充電のみ（録音しない）の赤は charge_only_mode() で点ける。
void set_led_state()
{
	bool charging = usb_power_present();
	bool lit = is_connected || led_blink_phase;
	led_blink_phase = !led_blink_phase;

	set_led_red(false);
	set_led_green(charging && lit);
	set_led_blue(!charging && lit);
}

static void charge_only_mode(void)
{
	printk("USB power detected: charge-only mode (mic off, Bluetooth off)\n");

	// マイクは起動しないが、電源ピンを明示的に Low にしておく
	mic_power_off();

	// 充電のみ（録音しない）= 赤
	set_led_green(false);
	set_led_blue(false);
	set_led_red(true);

	// 充電電流の設定は通常モード（transport_start 内）と同じにそろえる
	int battErr = 0;
	battErr |= battery_init();
	battErr |= battery_charge_start();
	if (battErr)
	{
		printk("Battery init failed (err %d)\n", battErr);
	}

	int64_t absent_since = -1;
	while (1)
	{
		if (usb_power_present())
		{
			absent_since = -1;
		}
		else if (absent_since < 0)
		{
			absent_since = k_uptime_get();
		}
		else if (k_uptime_get() - absent_since >= USB_REMOVED_DEBOUNCE_MS)
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
	if (usb_power_seen_at_boot())
	{
		charge_only_mode();
	}

	set_led_blue(true);

	// 初期化が途中で失敗しても main を抜けずに下の LED ループへ入る（本家は ASSERT_OK で return していた）
	int err = transport_start();
	if (err)
	{
		printk("Transport start failed (err %d)\n", err);
	}
	else
	{
		// Codec start
		set_codec_callback(codec_handler);
		err = codec_start();
		if (err)
		{
			printk("Codec start failed (err %d)\n", err);
		}
		else
		{
			// Mic start
			set_mic_callback(mic_handler);
			err = mic_start();
			if (err)
			{
				printk("Mic start failed (err %d)\n", err);
			}
		}
	}

	// 録音中は USB を挿しても止めない（録音しながら充電）
	while (1)
	{
		set_led_state();
		k_msleep(500);
	}

	// Unreachable
	return 0;
}
