// Includes:
#include "StackApp.h"
#include "StackLog.h"
#include "StackLaunchpadTrigger.h"
#include "StackGtkHelper.h"
#include "StackJson.h"
#include <list>
#include "alsa/asoundlib.h"

// Definitions - MIDI events:
#define MIDI_NOTE_ON          0x90
#define MIDI_CONTROL_CHANGE   0xb0
#define MIDI_SYSEX            0xf0
#define MIDI_SYSEX_END        0xf7

// Definitions: Global button indices:
#define GLOBAL_BUTTON_INDEX_UP       0
#define GLOBAL_BUTTON_INDEX_DOWN     1
#define GLOBAL_BUTTON_INDEX_LEFT     2
#define GLOBAL_BUTTON_INDEX_RIGHT    3
#define GLOBAL_BUTTON_INDEX_GO       4
#define GLOBAL_BUTTON_INDEX_STOP_ALL 5
#define GLOBAL_BUTTON_COUNT          6

// Typedefs: Details of a button on the device
typedef struct LaunchpadButton
{
	stack_time_t last_press_time;
	int32_t usage_count;
	int8_t r;
	int8_t g;
	int8_t b;
} LaunchpadButton;

// Typedefs: Details of the entire device
typedef struct LaunchpadDevice
{
	LaunchpadButton *buttons;
	snd_rawmidi_t *handle_in;
	snd_rawmidi_t *handle_out;
	struct pollfd poll_fds;
	uint8_t rows;
	uint8_t columns;
	bool ready;
} LaunchpadDevice;

// Typedefs: Details of the global buttons
typedef struct LaunchpadGlobalButton {
	uint8_t column;
	uint8_t row;
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint32_t keymap;
} LaunchpadGlobalButton;

// The list of active triggers for the thread
std::list<StackLaunchpadTrigger*> trigger_list;

// The mutex lock around our list (might not be necessary as everything happens
// on the UI thread)
std::mutex list_mutex;

// The single thread
std::thread midi_thread;

// Whether the thread is running
bool thread_running = false;

// Details of all the global buttons
LaunchpadGlobalButton global_buttons[GLOBAL_BUTTON_COUNT] = {
	{1, 1, 255, 255, 255, GDK_KEY_Up},
	{2, 1, 255, 255, 255, GDK_KEY_Down},
	{3, 1, 255, 255, 255, GDK_KEY_Left},
	{4, 1, 255, 255, 255, GDK_KEY_Right},
	{9, 9,   0, 255, 0,   GDK_KEY_space},
	{9, 6, 255,   0, 0,   GDK_KEY_Escape}
};

////////////////////////////////////////////////////////////////////////////////
// THREAD FUNCTIONS

static bool stack_launchpad_trigger_get_device_address(char *device_address, size_t device_address_length)
{
	int err;
	bool found = false;
	int card = -1;

	// Iterate over all sound cards
	while (snd_card_next(&card) >= 0 && card != -1)
	{
		snd_ctl_t *ctl;
		char card_device[32];
		snprintf(card_device, sizeof(card_device), "hw:%d", card);
		err = snd_ctl_open(&ctl, card_device, 0);
		if (err < 0)
		{
			break;
		}

		while (!found)
		{
			int dev;

			// Get the next device
			if ((err = snd_ctl_rawmidi_next_device(ctl, &dev)) < 0)
			{
				break;
			}

			// If we've reached the last device, break out of the loop
			if (dev < 0)
			{
				break;
			}

			// Allocate the info structure
			snd_rawmidi_info_t *info;
			snd_rawmidi_info_malloc(&info);

			// Get information about the device
			snd_rawmidi_info_set_device(info, dev);
			snd_rawmidi_info_set_subdevice(info, 0);
			snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
			if ((err = snd_ctl_rawmidi_info(ctl, info)) < 0)
			{
				snd_rawmidi_info_free(info);
				if (err != -ENOENT)
				{
					break;
				}
				continue;
			}

			const char *stream_id = snd_rawmidi_info_get_id(info);
			const char *name = snd_rawmidi_info_get_name(info);

			// Skip past non-Launchpad devices
			if (strstr(name, "Launchpad") != NULL)
			{
				int subdevices_count = snd_rawmidi_info_get_subdevices_count(info);

				for (int subdev = 0; subdev < subdevices_count; subdev++)
				{
					snd_rawmidi_info_set_subdevice(info, subdev);
					if ((err = snd_ctl_rawmidi_info(ctl, info)) < 0)
					{
						break;
					}
					const char *subdevice_name = snd_rawmidi_info_get_subdevice_name(info);

					// Skip past non-Launchpad and non-MIDI (i.e. DAW) devices
					if (strstr(subdevice_name, "Launchpad") != NULL && strstr(subdevice_name, " MIDI ") != NULL)
					{
						stack_log("stack_launchpad_trigger_get_device_address(): Launchpad found at hw:%d,%d,%d\n", card, dev, subdev);
						snprintf(device_address, device_address_length, "hw:%d,%d,%d", card, dev, subdev);
						found = true;
						break;
					}
				}
			}

			snd_rawmidi_info_free(info);
		}

		// Tidy up
		snd_ctl_close(ctl);
	}

	return found;
}

static void stack_launchpad_trigger_address_to_col_row(unsigned char address, uint8_t *column, uint8_t *row)
{
	*row = 10 - ((address - 1) / 10);
	*column = (address % 10);
}

// Returns the address of an LED based on its column/row (1-9)
static unsigned char stack_launchpad_trigger_col_row_to_address(uint8_t column, uint8_t row)
{
	return (10 - row) * 10 + column;
}

static LaunchpadButton *stack_launchpad_trigger_get_button(LaunchpadDevice *device, uint8_t column, uint8_t row)
{
	if (device == NULL || column > device->columns || row > device->rows)
	{
		return NULL;
	}

	return &device->buttons[(row - 1) * device->rows + column - 1];
}

// Sets the button at the given address to the give RGB color (where r, g, and b are 0 - 255)
static void stack_launchpad_trigger_midi_set_color(LaunchpadDevice *device, uint8_t column, uint8_t row, uint8_t r, uint8_t g, uint8_t b)
{
	LaunchpadButton *button = stack_launchpad_trigger_get_button(device, column, row);
	button->r = r;
	button->g = g;
	button->b = b;

	// Taken from the LED lighting SysEx message documentation found at
	// https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/downloads/Launchpad%20X%20-%20Programmers%20Reference%20Manual.pdf
	unsigned char output[13] = {MIDI_SYSEX, 0x00, 0x20, 0x29, 0x02, 0x0C, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, MIDI_SYSEX_END};
	output[8] = stack_launchpad_trigger_col_row_to_address(column, row);
	// MIDI colours are from 0-127 (so as to keep the MSB a zero)
	output[9] = r / 2;
	output[10] = g / 2;
	output[11] = b / 2;

	// Send the event
	if (device != NULL && device->ready)
	{
		snd_rawmidi_write(device->handle_out, output, 13);
		snd_rawmidi_drain(device->handle_out);
	}
}

// Increments a buttons usage count, and sets the colour of the button
static void stack_launchpad_trigger_add_button(LaunchpadDevice *device, uint8_t column, uint8_t row, int8_t r, int8_t g, int8_t b)
{
	LaunchpadButton *button = stack_launchpad_trigger_get_button(device, column, row);
	if (button != NULL)
	{
		button->usage_count++;
		stack_launchpad_trigger_midi_set_color(device, column, row, r, g, b);
	}

	// Check to see if any other triggers are using this button and update their color
	for (auto trigger : trigger_list)
	{
		if (trigger->column == column && trigger->row == row)
		{
			trigger->r = r;
			trigger->g = g;
			trigger->b = b;
		}
	}
}

// Decrements a buttons usage count and turns the button off if longer in use
static void stack_launchpad_trigger_remove_button(LaunchpadDevice *device, uint8_t column, uint8_t row)
{
	if (column == 0 || row == 0 || column > device->columns || row > device->rows)
	{
		return;
	}

	LaunchpadButton *button = stack_launchpad_trigger_get_button(device, column, row);
	if (button != NULL && button->usage_count > 0)
	{
		button->usage_count--;
		if (button->usage_count == 0)
		{
			stack_launchpad_trigger_midi_set_color(device, column, row, 0, 0, 0);
		}
	}
}

// Sends a single, giant SysEx message to set all the button colours at once
static void stack_launchpad_trigger_midi_refresh_colors(LaunchpadDevice *device)
{
	// Taken from the LED lighting SysEx message documentation found at
	// https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/downloads/Launchpad%20X%20-%20Programmers%20Reference%20Manual.pdf
	// 413 = 9 rows * 9 columns * 5 bytes per button + 7 byte command prefix + 1 byte command suffix
	unsigned char output[413] = {MIDI_SYSEX, 0x00, 0x20, 0x29, 0x02, 0x0C, 0x03};

	// Set all the addresses
	size_t offset = 7;
	for (size_t row = 1; row <= device->rows; row++)
	{
		for (size_t column = 1; column <= device->columns; column++)
		{
			LaunchpadButton *button = stack_launchpad_trigger_get_button(device, column, row);

			output[offset + 0] = 0x03;
			output[offset + 1] = stack_launchpad_trigger_col_row_to_address(column, row);
			output[offset + 2] = button->r / 2;
			output[offset + 3] = button->g / 2;
			output[offset + 4] = button->b / 2;
			offset += 5;
		}
	}
	output[412] = MIDI_SYSEX_END;

	// Send the event
	if (device->ready)
	{
		snd_rawmidi_write(device->handle_out, output, 413);
		snd_rawmidi_drain(device->handle_out);
	}
}

// Sets all the colors in the grid to black
static void stack_launchpad_trigger_midi_all_off(LaunchpadDevice *device)
{
	// Set all the colors in our local grid
	for (size_t row = 1; row <= device->rows; row++)
	{
		for (size_t column = 1; column <= device->columns; column++)
		{
			LaunchpadButton *button = stack_launchpad_trigger_get_button(device, column, row);
			button->r = 0;
			button->g = 0;
			button->b = 0;
		}
	}

	// Send all the updates in one message
	stack_launchpad_trigger_midi_refresh_colors(device);
}

// Updates the buttons array with the current colours for all triggers
static void stack_launchpad_trigger_update_buttons(LaunchpadDevice *device)
{
	// Reset
	for (size_t row = 1; row <= device->rows; row++)
	{
		for (size_t column = 1; column <= device->columns; column++)
		{
			LaunchpadButton *button = stack_launchpad_trigger_get_button(device, column, row);
			button->r = 0;
			button->g = 0;
			button->b = 0;
			button->usage_count = 0;
		}
	}

	// Set the buttons for active triggers
	for (auto trigger : trigger_list)
	{
		LaunchpadButton *button = stack_launchpad_trigger_get_button(device, trigger->column, trigger->row);
		button->usage_count++;

		stack_launchpad_trigger_midi_set_color(device, trigger->column, trigger->row, trigger->r, trigger->g, trigger->b);

		if (trigger->use_for_cue_list)
		{
			// Add all our global buttons
			for (size_t i = 0; i < GLOBAL_BUTTON_COUNT; i++)
			{
				stack_launchpad_trigger_add_button(device,
					global_buttons[i].column,
					global_buttons[i].row,
					global_buttons[i].r,
					global_buttons[i].g,
					global_buttons[i].b);
			}
		}
	}
}

static LaunchpadDevice *stack_launchpad_trigger_get_device(bool create)
{
	static LaunchpadDevice device;
	static bool initialised = false;
	static bool shown_missing_error = false;

	// Lock whilst we set up the device just so there's no chance of a second
	// device being created
	list_mutex.lock();

	if (!initialised)
	{
		device.handle_in = NULL;
		device.handle_out = NULL;
		device.ready = false;
		initialised = true;
		device.rows = 9;
		device.columns = 9;
		device.buttons = new LaunchpadButton[device.rows * device.columns];
		memset(device.buttons, 0, device.columns * device.rows * sizeof(LaunchpadButton));
	}

	// If we already have a device, return it
	if (device.ready)
	{
		list_mutex.unlock();
		return &device;
	}

	// If we've not get a ready device, but we're not creating one, return NULL
	if (!create)
	{
		list_mutex.unlock();
		return &device;
	}

	// Find the address of the Launchpad device
	char device_address[32];
	if (!stack_launchpad_trigger_get_device_address(device_address, sizeof(device_address)))
	{
		// Only show this error once
		if (!shown_missing_error)
		{
			stack_log("stack_launchpad_trigger_get_device(): No Launchpad MIDI device found!\n");
			shown_missing_error = true;
		}
		list_mutex.unlock();
		return &device;
	}

	// Open the MIDI in/out devices
	if (!device.ready)
	{
		stack_log("stack_launchpad_trigger_get_device(): Opening new device\n");
		int result = snd_rawmidi_open(&device.handle_in, &device.handle_out, device_address, 0);
		if (result > 0)
		{
			stack_log("stack_launchpad_trigger_get_device(): Failed to open MIDI devices: %d\n", result);
			list_mutex.unlock();
			return &device;
		}

		int count = snd_rawmidi_poll_descriptors(device.handle_in, &device.poll_fds, 1);
		if (count == 0)
		{
			stack_log("stack_launchpad_trigger_get_device(): Failed to get MIDI poll descriptors\n");
			snd_rawmidi_close(device.handle_in);
			snd_rawmidi_close(device.handle_out);
			list_mutex.unlock();
			return &device;
		}

		shown_missing_error = false;
		device.ready = true;

		// Ensure all the LEDs are set correctly
		stack_launchpad_trigger_update_buttons(&device);
	}

	// Unlock again now that we're ready
	list_mutex.unlock();

	return &device;
}

static void stack_launchpad_trigger_close_device(LaunchpadDevice *device)
{
	if (device == NULL)
	{
		return;
	}

	if (device->handle_out != NULL)
	{
		// Clear all the buttons at once
		stack_launchpad_trigger_midi_all_off(device);

		snd_rawmidi_drain(device->handle_out);
		snd_rawmidi_close(device->handle_out);
		stack_log("stack_launchpad_trigger_close_device(): MIDI Out closed\n");
		device->handle_out = NULL;
	}

	device->ready = false;

	if (device->handle_in != NULL)
	{
		snd_rawmidi_close(device->handle_in);
		stack_log("stack_launchpad_trigger_close_device(): MIDI In closed\n");
		device->handle_in = NULL;
	}

	// We only ever initialise the button array once, so don't delete it
	/*if (device->buttons != NULL)
	{
		delete [] device->buttons;
		device->buttons = NULL;
	}
	device->rows = 0;
	device->columns = 0;*/
}

static void stack_launchpad_trigger_run_action(StackLaunchpadTrigger *trigger)
{
	// Get the cue and the action
	StackCue *cue = STACK_TRIGGER(trigger)->cue;
	StackTriggerAction action = stack_trigger_get_action(STACK_TRIGGER(trigger));

	// Run the correct action
	stack_cue_list_lock(cue->parent);
	switch (action)
	{
		case STACK_TRIGGER_ACTION_STOP:
			stack_cue_stop(cue);
			break;
		case STACK_TRIGGER_ACTION_PAUSE:
			stack_cue_pause(cue);
			break;
		case STACK_TRIGGER_ACTION_PLAY:
			stack_cue_play(cue);
			break;
	}
	stack_cue_list_unlock(cue->parent);
}

typedef struct SimKeyData
{
	GdkEvent *event;
	GObject *widget;
} SimKeyData;

static gboolean stack_launchpad_trigger_fake_keypress(gpointer f)
{
	gboolean result;
	g_signal_emit_by_name(((SimKeyData*)f)->widget, "key-press-event", ((SimKeyData*)f)->event, &result);
	delete (SimKeyData*)f;
	return G_SOURCE_REMOVE;
}

static void stack_launchpad_trigger_simulate_keypress(StackAppWindow *window, guint key)
{
	GdkEvent *event = gdk_event_new(GDK_KEY_PRESS);
	event->key.window = g_object_ref(window->sclw->window);
	event->key.send_event = TRUE;
	event->key.time = 0;
	event->key.state = 0;
	event->key.hardware_keycode = 0;
	event->key.group = 0;
	event->key.is_modifier = 0;
	event->key.length = 0;
	event->key.keyval = key;
	event->key.string = g_strdup("");

	SimKeyData *data = new SimKeyData;
	data->event = event;
	data->widget = G_OBJECT(window->sclw);
	gdk_threads_add_idle(stack_launchpad_trigger_fake_keypress, data);
}

static void stack_launchpad_trigger_thread(void *user_data)
{
	int result = 0;
	LaunchpadDevice *device = NULL;

	// Keep the thread about whilst we have triggers to process
	while (trigger_list.size() > 0)
	{
		while (trigger_list.size() > 0 && (device == NULL || !device->ready))
		{
			device = stack_launchpad_trigger_get_device(true);
			if (device == NULL || !device->ready)
			{
				std::this_thread::sleep_for(std::chrono::seconds(1));
			}
		}

		// Read from the MIDI device
		int poll_result = poll(&device->poll_fds, 1, 100);
		if (poll_result < 0)
		{
			stack_log("stack_launchpad_trigger_thread(): Poll failed: %d\n", poll_result);
			continue;
		}
		else if (poll_result == 0)
		{
			// Nothing was ready
			continue;
		}

		// Device could have returned from the poll after it was closed
		if (!device->ready)
		{
			continue;
		}

		unsigned char buf[32];
		result = snd_rawmidi_read(device->handle_in, buf, sizeof(buf));
		if (result < 0)
		{
			// If we fail to read, close the device so that we can retry
			stack_log("stack_launchpad_trigger_thread(): Failed to read from MIDI device: %d\n", result);
			stack_launchpad_trigger_close_device(device);
			continue;
		}

		// Iterate over all the bytes in the message
		for (size_t i = 0; i < result; i++)
		{
			const unsigned char event = buf[i];
			const unsigned char address = buf[i + 1];
			const unsigned char pressure = buf[i + 2];
			unsigned char r = 127, g = 0, b = 0, column = 0, row = 0;

			// All MIDI messages start with a byte whose MSB is 1, so sync up to that
			if (event & 0x80 != 0)
			{
				continue;
			}

			// Button presses are either a note on or a controller change
			if (event != MIDI_NOTE_ON && event != MIDI_CONTROL_CHANGE)
			{
				continue;
			}

			// Get the button
			stack_launchpad_trigger_address_to_col_row(address, &column, &row);
			LaunchpadButton *button = stack_launchpad_trigger_get_button(device, column, row);

			// Search for the trigger for this button (if there is one)
			list_mutex.lock();
			for (auto trigger : trigger_list)
			{
				if (trigger->use_for_cue_list)
				{
					uint8_t r = 0, g = 0, b = 0;

					uint32_t keyval = 0;
					for (size_t i = 0; i < GLOBAL_BUTTON_COUNT; i++)
					{
						if (row == global_buttons[i].row && column == global_buttons[i].column)
						{
							keyval = global_buttons[i].keymap;
							r = global_buttons[i].r;
							g = global_buttons[i].g;
							b = global_buttons[i].b;
						}
					}

					if (keyval > 0)
					{
						if (pressure > 0)
						{
							if (stack_get_clock_time() - button->last_press_time > 1e3)
							{
								button->last_press_time = stack_get_clock_time();
								stack_launchpad_trigger_midi_set_color(device, column, row, 0, 0, 0);

								StackAppWindow *window = saw_get_window_for_cue(STACK_TRIGGER(trigger)->cue);
								if (keyval != GDK_KEY_Escape)
								{
									stack_launchpad_trigger_simulate_keypress(window, keyval);
								}
								else
								{
									// We have a function for this one
									stack_cue_list_stop_all(trigger->super.cue->parent);
								}
							}
							device->buttons[row * device->rows + column].last_press_time = stack_get_clock_time();
							break;
						}
						else
						{
							stack_launchpad_trigger_midi_set_color(device, column, row, r, g, b);
						}
					}
				}
				if (trigger->row == row && trigger->column == column)
				{
					bool debounced = false;

					// To make it clear the button press is registered, turn off when pressed
					// and restore when released
					if (pressure > 0)
					{
						if (stack_get_clock_time() - button->last_press_time > 1e3)
						{
							button->last_press_time = stack_get_clock_time();
							stack_launchpad_trigger_midi_set_color(device, column, row, 0, 0, 0);
						}
						else
						{
							debounced = true;
						}
					}
					else
					{
						stack_launchpad_trigger_midi_set_color(device, column, row, trigger->r, trigger->g, trigger->b);
					}

					if ((!debounced && pressure > 0 && trigger->on_pressed) || (pressure == 0 && !trigger->on_pressed))
					{
						stack_launchpad_trigger_run_action(trigger);
					}
				}
			}
			list_mutex.unlock();
		}
	}

	// Tidy up
	stack_launchpad_trigger_close_device(device);

	// Note the exit of the thread
	stack_log("stack_launchpad_trigger_thread(): Terminating\n");
	thread_running = false;
}

////////////////////////////////////////////////////////////////////////////////
// CREATION AND DESTRUCTION

/// Creates a key trigger
StackTrigger* stack_launchpad_trigger_create(StackCue *cue)
{
	// Allocate the trigger
	StackLaunchpadTrigger *trigger = new StackLaunchpadTrigger();

	// Initialise the superclass
	stack_trigger_init(&trigger->super, cue);

	// Make this class a StackLaunchpadTrigger
	STACK_TRIGGER(trigger)->_class_name = "StackLaunchpadTrigger";

	// Initial setup
	trigger->description = strdup("");
	trigger->event_text[0] = '\0';
	trigger->r = 0;
	trigger->g = 0;
	trigger->b = 0;
	trigger->column = 0;
	trigger->row = 0;
	trigger->on_pressed = true;
	trigger->use_for_cue_list = false;

	// Add us to the list of triggers
	list_mutex.lock();
	trigger_list.push_back(trigger);

	if (!thread_running)
	{
		thread_running = true;
		stack_log("stack_launchpad_trigger_create(): Creating thread\n");

		// Join the (presumably) dead thread if it's still joinable, so we don't
		// throw an exception when creating the new one
		if (midi_thread.joinable())
		{
			midi_thread.join();
		}

		midi_thread = std::thread(stack_launchpad_trigger_thread, (void*)NULL);
	}

	// We're done with list actions now
	list_mutex.unlock();

	return STACK_TRIGGER(trigger);
}

/// Destroys a key trigger
void stack_launchpad_trigger_destroy(StackTrigger *trigger)
{
	StackLaunchpadTrigger *launchpad_trigger = STACK_LAUNCHPAD_TRIGGER(trigger);

	// Get the current device (but don't create if we don't have one)
	LaunchpadDevice *device = stack_launchpad_trigger_get_device(false);

	// Remove ourselves from the list
	list_mutex.lock();
	trigger_list.remove(launchpad_trigger);

	// Mark the button as no longer in use
	if (device != NULL && launchpad_trigger->column > 0 && launchpad_trigger->row > 0)
	{
		stack_launchpad_trigger_remove_button(device, launchpad_trigger->column, launchpad_trigger->row);
	}

	// Wait for the thread to die
	if (trigger_list.size() == 0)
	{
		stack_log("stack_launchpad_trigger_destroy(): No triggers left, closing device\n");

		// Close device locks the mutex, so unlock here first
		list_mutex.unlock();
		if (device != NULL)
		{
			stack_launchpad_trigger_close_device(device);
		}
		stack_log("stack_launchpad_trigger_destroy(): Waiting for thread\n");
		midi_thread.join();
	}
	else
	{
		list_mutex.unlock();
	}

	if (launchpad_trigger->description != NULL)
	{
		free(launchpad_trigger->description);
	}

	// Call parent destructor
	stack_trigger_destroy_base(trigger);
}

////////////////////////////////////////////////////////////////////////////////
// OVERRIDDEN FUNCTIONS

// Return either "Key Pressed" or "Key Released" as the text depending on what
// the trigger is configured for
const char* stack_launchpad_trigger_get_name(StackTrigger *trigger)
{
	if (STACK_LAUNCHPAD_TRIGGER(trigger)->on_pressed)
	{
		return "Launchpad Pressed";
	}
	else
	{
		return "Launchpad Released";
	}
}

// Returns the name of the key we're triggered off
const char* stack_launchpad_trigger_get_event_text(StackTrigger *trigger)
{
	StackLaunchpadTrigger *launchpad_trigger = STACK_LAUNCHPAD_TRIGGER(trigger);

	snprintf(launchpad_trigger->event_text, sizeof(launchpad_trigger->event_text), "Button (%d, %d)", launchpad_trigger->column, launchpad_trigger->row);
	return launchpad_trigger->event_text;
}

// Returns the user-specified description
const char* stack_launchpad_trigger_get_description(StackTrigger *trigger)
{
	return STACK_LAUNCHPAD_TRIGGER(trigger)->description;
}

char *stack_launchpad_trigger_to_json(StackTrigger *trigger)
{
	Json::Value trigger_root;
	StackLaunchpadTrigger *launchpad_trigger = STACK_LAUNCHPAD_TRIGGER(trigger);

	trigger_root["description"] = launchpad_trigger->description;
	trigger_root["row"] = launchpad_trigger->row;
	trigger_root["column"] = launchpad_trigger->column;
	trigger_root["r"] = launchpad_trigger->r;
	trigger_root["g"] = launchpad_trigger->g;
	trigger_root["b"] = launchpad_trigger->b;
	trigger_root["on_pressed"] = launchpad_trigger->on_pressed;
	trigger_root["use_for_cue_list"] = launchpad_trigger->use_for_cue_list;

	Json::StreamWriterBuilder builder;
	return strdup(Json::writeString(builder, trigger_root).c_str());
}

void stack_launchpad_trigger_free_json(StackTrigger *trigger, char *json_data)
{
	free(json_data);
}

void stack_launchpad_trigger_from_json(StackTrigger *trigger, const char *json_data)
{
	Json::Value trigger_root;

	// Call the superclass version
	stack_trigger_from_json_base(trigger, json_data);

	// Parse JSON data
	stack_json_read_string(json_data, &trigger_root);

	// Get the data that's pertinent to us
	Json::Value& trigger_data = trigger_root["StackLaunchpadTrigger"];

	StackLaunchpadTrigger *launchpad_trigger = STACK_LAUNCHPAD_TRIGGER(trigger);
	if (trigger_data.isMember("description"))
	{
		if (launchpad_trigger->description != NULL)
		{
			free(launchpad_trigger->description);
		}
		launchpad_trigger->description = strdup(trigger_data["description"].asString().c_str());
	}

	if (trigger_data.isMember("row"))
	{
		launchpad_trigger->row = trigger_data["row"].asUInt();
	}

	if (trigger_data.isMember("column"))
	{
		launchpad_trigger->column = trigger_data["column"].asUInt();
	}

	if (trigger_data.isMember("r"))
	{
		launchpad_trigger->r = trigger_data["r"].asUInt();
	}

	if (trigger_data.isMember("g"))
	{
		launchpad_trigger->g = trigger_data["g"].asUInt();
	}

	if (trigger_data.isMember("b"))
	{
		launchpad_trigger->b = trigger_data["b"].asUInt();
	}

	if (trigger_data.isMember("on_pressed"))
	{
		launchpad_trigger->on_pressed = trigger_data["on_pressed"].asBool();
	}

	if (trigger_data.isMember("use_for_cue_list"))
	{
		launchpad_trigger->use_for_cue_list = trigger_data["use_for_cue_list"].asBool();
	}

	LaunchpadDevice *device = stack_launchpad_trigger_get_device(true);
	if (device)
	{
		stack_launchpad_trigger_add_button(device, launchpad_trigger->column, launchpad_trigger->row, launchpad_trigger->r, launchpad_trigger->g, launchpad_trigger->b);
	}
}

////////////////////////////////////////////////////////////////////////////////
// CONFIGURATION USER INTERFACE

void stack_launchpad_trigger_set_global_button_ui(GtkBuilder *builder, const char *name, LaunchpadGlobalButton *button)
{
	char builder_object_name[64], buffer[16];
	GtkEntry *entry = NULL;

	// Set up column entry
	snprintf(builder_object_name, 64, "ltgsd%sColumnEntry", name);
	entry = GTK_ENTRY(gtk_builder_get_object(builder, builder_object_name));
	stack_limit_gtk_entry_int(entry, false);
	snprintf(buffer, 16, "%d", button->column);
	gtk_entry_set_text(entry, buffer);

	// Set up row entry
	snprintf(builder_object_name, 64, "ltgsd%sRowEntry", name);
	entry = GTK_ENTRY(gtk_builder_get_object(builder, builder_object_name));
	stack_limit_gtk_entry_int(entry, false);
	snprintf(buffer, 16, "%d", button->row);
	gtk_entry_set_text(entry, buffer);

	// Set up color widget
	snprintf(builder_object_name, 64, "ltgsd%sColorChooser", name);
	GtkColorChooser *chooser = GTK_COLOR_CHOOSER(gtk_builder_get_object(builder, builder_object_name));
	GdkRGBA color = {
		(double)button->r / 255.0,
		(double)button->g / 255.0,
		(double)button->b / 255.0,
		1.0
	};
	gtk_color_chooser_set_rgba(chooser, &color);
}

bool stack_launchpad_trigger_get_global_button_from_ui(GtkDialog *parent, GtkBuilder *builder, const char *name, LaunchpadGlobalButton *button)
{
	char builder_object_name[64], buffer[16];
	GtkEntry *entry = NULL;

	// Set up column entry
	snprintf(builder_object_name, 64, "ltgsd%sColumnEntry", name);
	entry = GTK_ENTRY(gtk_builder_get_object(builder, builder_object_name));
	int new_column = atoi(gtk_entry_get_text(entry));
	if (new_column < 1 || new_column > 9)
	{
		GtkWidget *message_dialog = gtk_message_dialog_new(GTK_WINDOW(parent), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Invalid configuration");
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(message_dialog), "Button column must be between 1 and 9");
		gtk_window_set_title(GTK_WINDOW(message_dialog), "Error");
		gtk_dialog_run(GTK_DIALOG(message_dialog));
		gtk_widget_destroy(message_dialog);
		gtk_widget_grab_focus(GTK_WIDGET(entry));
		return false;
	}

	// Set up row entry
	snprintf(builder_object_name, 64, "ltgsd%sRowEntry", name);
	entry = GTK_ENTRY(gtk_builder_get_object(builder, builder_object_name));
	int new_row = atoi(gtk_entry_get_text(entry));
	if (new_row < 1 || new_row > 9)
	{
		GtkWidget *message_dialog = gtk_message_dialog_new(GTK_WINDOW(parent), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Invalid configuration");
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(message_dialog), "Button row must be between 1 and 9");
		gtk_window_set_title(GTK_WINDOW(message_dialog), "Error");
		gtk_dialog_run(GTK_DIALOG(message_dialog));
		gtk_widget_destroy(message_dialog);
		gtk_widget_grab_focus(GTK_WIDGET(entry));
		return false;
	}

	// Set up color widget
	snprintf(builder_object_name, 64, "ltgsd%sColorChooser", name);
	GtkColorChooser *chooser = GTK_COLOR_CHOOSER(gtk_builder_get_object(builder, builder_object_name));
	stack_gtk_color_chooser_get_rgb(chooser, &button->r, &button->g, &button->b);

	// Update the object
	button->column = new_column;
	button->row = new_row;

	return true;
}

gboolean stack_launchpad_trigger_global_settings_clicked(GtkWidget *widget, gpointer user_data)
{
	// Build the dialog
	GtkBuilder *builder = gtk_builder_new_from_resource("/org/stack/ui/StackLaunchpadGlobalSettings.ui");
	GtkDialog *dialog = GTK_DIALOG(gtk_builder_get_object(builder, "launchpadTriggerGlobalSettingsDialog"));
	gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(user_data));

	// Set up response buttons
	gtk_dialog_add_buttons(dialog, "Cancel", 2, "OK", 1, NULL);
	gtk_dialog_set_default_response(dialog, 1);

	// Set up all the buttons
	stack_launchpad_trigger_set_global_button_ui(builder, "Up", &global_buttons[GLOBAL_BUTTON_INDEX_UP]);
	stack_launchpad_trigger_set_global_button_ui(builder, "Down", &global_buttons[GLOBAL_BUTTON_INDEX_DOWN]);
	stack_launchpad_trigger_set_global_button_ui(builder, "Left", &global_buttons[GLOBAL_BUTTON_INDEX_LEFT]);
	stack_launchpad_trigger_set_global_button_ui(builder, "Right", &global_buttons[GLOBAL_BUTTON_INDEX_RIGHT]);
	stack_launchpad_trigger_set_global_button_ui(builder, "Go", &global_buttons[GLOBAL_BUTTON_INDEX_GO]);
	stack_launchpad_trigger_set_global_button_ui(builder, "StopAll", &global_buttons[GLOBAL_BUTTON_INDEX_STOP_ALL]);

	bool loop = false;
	do
	{
		// Run the dialog
		loop = false;
		gint response = gtk_dialog_run(dialog);

		// OK pressed
		if (response == 1)
		{
			LaunchpadGlobalButton new_buttons[GLOBAL_BUTTON_COUNT];
			memcpy(new_buttons, global_buttons, sizeof(LaunchpadGlobalButton) * GLOBAL_BUTTON_COUNT);

			// Check and get values for all the buttons
			if (!stack_launchpad_trigger_get_global_button_from_ui(dialog, builder, "Up", &new_buttons[GLOBAL_BUTTON_INDEX_UP]) ||
			    !stack_launchpad_trigger_get_global_button_from_ui(dialog, builder, "Down", &new_buttons[GLOBAL_BUTTON_INDEX_DOWN]) ||
			    !stack_launchpad_trigger_get_global_button_from_ui(dialog, builder, "Left", &new_buttons[GLOBAL_BUTTON_INDEX_LEFT]) ||
			    !stack_launchpad_trigger_get_global_button_from_ui(dialog, builder, "Right", &new_buttons[GLOBAL_BUTTON_INDEX_RIGHT]) ||
			    !stack_launchpad_trigger_get_global_button_from_ui(dialog, builder, "Go", &new_buttons[GLOBAL_BUTTON_INDEX_GO]) ||
			    !stack_launchpad_trigger_get_global_button_from_ui(dialog, builder, "StopAll", &new_buttons[GLOBAL_BUTTON_INDEX_STOP_ALL]))
			{
				loop = true;
			}

			// If everything was fine, copy the data to our global array and update
			// the buttoms
			if (!loop)
			{
				memcpy(global_buttons, new_buttons, sizeof(LaunchpadGlobalButton) * GLOBAL_BUTTON_COUNT);
				stack_launchpad_trigger_update_buttons(stack_launchpad_trigger_get_device(true));
			}
		}
	} while (loop);

	// Destroy the dialog
	gtk_widget_destroy(GTK_WIDGET(dialog));

	// Free the builder
	g_object_unref(builder);

	return false;
}

bool stack_launchpad_trigger_show_config_ui(StackTrigger *trigger, GtkWidget *parent, bool new_trigger)
{
	bool result = false;
	StackLaunchpadTrigger *launchpad_trigger = STACK_LAUNCHPAD_TRIGGER(trigger);

	// Build the dialog
	GtkBuilder *builder = gtk_builder_new_from_resource("/org/stack/ui/StackLaunchpadTrigger.ui");
	GtkDialog *dialog = GTK_DIALOG(gtk_builder_get_object(builder, "launchpadTriggerDialog"));
	gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parent));

	// Callbacks
	gtk_builder_add_callback_symbol(builder, "stack_launchpad_trigger_global_settings_clicked", G_CALLBACK(stack_launchpad_trigger_global_settings_clicked));
	gtk_builder_connect_signals(builder, dialog);

	// Set up response buttons
	gtk_dialog_add_buttons(dialog, "Cancel", 2, "OK", 1, NULL);
	gtk_dialog_set_default_response(dialog, 1);

    // We use these a lot
    GtkEntry *ltdDescriptionEntry = GTK_ENTRY(gtk_builder_get_object(builder, "ltdDescriptionEntry"));
    GtkEntry *ltdColumnEntry = GTK_ENTRY(gtk_builder_get_object(builder, "ltdColumnEntry"));
    GtkEntry *ltdRowEntry = GTK_ENTRY(gtk_builder_get_object(builder, "ltdRowEntry"));
    GtkColorButton *ltdColorButton = GTK_COLOR_BUTTON(gtk_builder_get_object(builder, "ltdColorButton"));
    GtkToggleButton *ltdActionStop = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "ltdActionStop"));
    GtkToggleButton *ltdActionPause = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "ltdActionPause"));
    GtkToggleButton *ltdActionPlay = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "ltdActionPlay"));
    GtkToggleButton *ltdEventPress = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "ltdEventPress"));
    GtkToggleButton *ltdEventRelease = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "ltdEventRelease"));
    GtkToggleButton *ltdCueListCheck = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "ltdCueListCheck"));

	// Set helpers
	stack_limit_gtk_entry_int(ltdColumnEntry, false);
	stack_limit_gtk_entry_int(ltdRowEntry, false);

	// Set the values on the dialog
	gtk_entry_set_text(ltdDescriptionEntry, launchpad_trigger->description);
	char buffer[64];
	if (launchpad_trigger->row != 0)
	{
		snprintf(buffer, 64, "%d", launchpad_trigger->column);
		gtk_entry_set_text(ltdColumnEntry, buffer);
	}
	if (launchpad_trigger->row != 0)
	{
		snprintf(buffer, 64, "%d", launchpad_trigger->row);
		gtk_entry_set_text(ltdRowEntry, buffer);
	}
	GdkRGBA rgba = {(double)launchpad_trigger->r / 255.0, (double)launchpad_trigger->g / 255.0, (double)launchpad_trigger->b / 255.0, 1.0};
	gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(ltdColorButton), &rgba);

    switch (trigger->action)
    {
        case STACK_TRIGGER_ACTION_STOP:
            gtk_toggle_button_set_active(ltdActionStop, true);
            break;
        case STACK_TRIGGER_ACTION_PAUSE:
            gtk_toggle_button_set_active(ltdActionPause, true);
            break;
        default:
        case STACK_TRIGGER_ACTION_PLAY:
            gtk_toggle_button_set_active(ltdActionPlay, true);
            break;
    }

	if (launchpad_trigger->on_pressed)
	{
        gtk_toggle_button_set_active(ltdEventPress, true);
	}
	else
	{
        gtk_toggle_button_set_active(ltdEventRelease, true);
	}

	gtk_toggle_button_set_active(ltdCueListCheck, launchpad_trigger->use_for_cue_list);

	bool loop;
	do
	{
		// Run the dialog
		loop = true;
		gint response = gtk_dialog_run(dialog);

		switch (response)
		{
			case 1:	// OK
				int column;
				column = atoi(gtk_entry_get_text(ltdColumnEntry));
				if (column < 1 || column > 9)
				{
					GtkWidget *message_dialog = NULL;
					message_dialog = gtk_message_dialog_new(GTK_WINDOW(parent), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Invalid configuration");
					gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(message_dialog), "Button column must be between 1 and 9");
					gtk_window_set_title(GTK_WINDOW(message_dialog), "Error");
					gtk_dialog_run(GTK_DIALOG(message_dialog));
					gtk_widget_destroy(message_dialog);
					continue;
				}

				int row;
				row = atoi(gtk_entry_get_text(ltdRowEntry));
				if (row < 1 || row > 9)
				{
					GtkWidget *message_dialog = NULL;
					message_dialog = gtk_message_dialog_new(GTK_WINDOW(parent), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Invalid configuration");
					gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(message_dialog), "Button row must be between 1 and 9");
					gtk_window_set_title(GTK_WINDOW(message_dialog), "Error");
					gtk_dialog_run(GTK_DIALOG(message_dialog));
					gtk_widget_destroy(message_dialog);
					continue;
				}

				// Before we update the values, remove the old button
				LaunchpadDevice *device;
				device = stack_launchpad_trigger_get_device(true);
				if (device != NULL)
				{
					stack_launchpad_trigger_remove_button(device, launchpad_trigger->column, launchpad_trigger->row);
				}

				// Update the position
				launchpad_trigger->column = column;
				launchpad_trigger->row = row;

				// Store the action
				if (gtk_toggle_button_get_active(ltdActionStop))
				{
					trigger->action = STACK_TRIGGER_ACTION_STOP;
				}
				else if (gtk_toggle_button_get_active(ltdActionPause))
				{
					trigger->action = STACK_TRIGGER_ACTION_PAUSE;
				}
				else if (gtk_toggle_button_get_active(ltdActionPlay))
				{
					trigger->action = STACK_TRIGGER_ACTION_PLAY;
				}

				// Update the description
				if (launchpad_trigger->description != NULL)
				{
					free(launchpad_trigger->description);
				}
				launchpad_trigger->description = strdup(gtk_entry_get_text(ltdDescriptionEntry));

				// Store the colour
				stack_gtk_color_chooser_get_rgb(GTK_COLOR_CHOOSER(ltdColorButton), &launchpad_trigger->r, &launchpad_trigger->g, &launchpad_trigger->b);

				// Store the event
				if (gtk_toggle_button_get_active(ltdEventPress))
				{
					launchpad_trigger->on_pressed = true;
				}
				else if (gtk_toggle_button_get_active(ltdEventRelease))
				{
					launchpad_trigger->on_pressed = false;
				}

				// Store the cue list controls
				bool old_cue_list_controls;
				old_cue_list_controls = launchpad_trigger->use_for_cue_list;
				launchpad_trigger->use_for_cue_list = gtk_toggle_button_get_active(ltdCueListCheck);

				// Re-add the button
				if (device != NULL)
				{
					stack_launchpad_trigger_add_button(device, launchpad_trigger->column, launchpad_trigger->row, launchpad_trigger->r, launchpad_trigger->g, launchpad_trigger->b);

					// If we've toggled cue list controls. refresh the entire panel
					if (old_cue_list_controls != launchpad_trigger->use_for_cue_list)
					{
						stack_launchpad_trigger_update_buttons(device);
					}
				}

				result = true;
				loop = false;
				break;
			case 2: // Cancel
				result = false;
				loop = false;
				break;
		}
	} while (loop);

	// Destroy the dialog
	gtk_widget_destroy(GTK_WIDGET(dialog));

	// Free the builder
	g_object_unref(builder);

	return result;
}

static void stack_launchpad_trigger_json_populate_button(Json::Value &value, LaunchpadGlobalButton *button)
{
	value["r"] = button->r;
	value["g"] = button->g;
	value["b"] = button->b;
	value["column"] = button->column;
	value["row"] = button->row;
}

char *stack_launchpad_trigger_config_to_json()
{
	Json::Value config_root;
	Json::Value buttons = Json::Value(Json::ValueType::objectValue);

	stack_launchpad_trigger_json_populate_button(buttons["up"], &global_buttons[GLOBAL_BUTTON_INDEX_UP]);
	stack_launchpad_trigger_json_populate_button(buttons["down"], &global_buttons[GLOBAL_BUTTON_INDEX_DOWN]);
	stack_launchpad_trigger_json_populate_button(buttons["left"], &global_buttons[GLOBAL_BUTTON_INDEX_LEFT]);
	stack_launchpad_trigger_json_populate_button(buttons["right"], &global_buttons[GLOBAL_BUTTON_INDEX_RIGHT]);
	stack_launchpad_trigger_json_populate_button(buttons["go"], &global_buttons[GLOBAL_BUTTON_INDEX_GO]);
	stack_launchpad_trigger_json_populate_button(buttons["stop_all"], &global_buttons[GLOBAL_BUTTON_INDEX_STOP_ALL]);
	config_root["global_buttons"] = buttons;

	Json::StreamWriterBuilder builder;
	std::string output = Json::writeString(builder, config_root);
	return strdup(output.c_str());
}

void stack_launchpad_trigger_config_free_json(char *json_data)
{
	free(json_data);
}

static void stack_launchpad_trigger_populate_button_from_json(Json::Value &buttons_root, const char *name, LaunchpadGlobalButton *button)
{
	if (buttons_root.isMember(name))
	{
		Json::Value &value = buttons_root[name];
		button->r = value["r"].asUInt();
		button->g = value["g"].asUInt();
		button->b = value["b"].asUInt();
		button->column = value["column"].asUInt();
		button->row = value["row"].asUInt();
	}
}

void stack_launchpad_trigger_config_from_json(const char *json_data)
{
	Json::Value config_root;
	stack_json_read_string(json_data, &config_root);

	if (config_root.isMember("global_buttons"))
	{
		Json::Value &buttons = config_root["global_buttons"];
		stack_launchpad_trigger_populate_button_from_json(buttons, "up", &global_buttons[GLOBAL_BUTTON_INDEX_UP]);
		stack_launchpad_trigger_populate_button_from_json(buttons, "down", &global_buttons[GLOBAL_BUTTON_INDEX_DOWN]);
		stack_launchpad_trigger_populate_button_from_json(buttons, "left", &global_buttons[GLOBAL_BUTTON_INDEX_LEFT]);
		stack_launchpad_trigger_populate_button_from_json(buttons, "right", &global_buttons[GLOBAL_BUTTON_INDEX_RIGHT]);
		stack_launchpad_trigger_populate_button_from_json(buttons, "go", &global_buttons[GLOBAL_BUTTON_INDEX_GO]);
		stack_launchpad_trigger_populate_button_from_json(buttons, "stop_all", &global_buttons[GLOBAL_BUTTON_INDEX_STOP_ALL]);
	}
}

////////////////////////////////////////////////////////////////////////////////
// CLASS REGISTRATION

// Registers StackLaunchpadTrigger with the application
void stack_launchpad_trigger_register()
{
	// Register built in cue types
	StackTriggerClass* launchpad_trigger_class = new StackTriggerClass{
		"StackLaunchpadTrigger",
		"StackTrigger",
		"Novation Launchpad",
		stack_launchpad_trigger_create,
		stack_launchpad_trigger_destroy,
		stack_launchpad_trigger_get_name,
		stack_launchpad_trigger_get_event_text,
		stack_launchpad_trigger_get_description,
		NULL, //get_action
		stack_launchpad_trigger_to_json,
		stack_launchpad_trigger_free_json,
		stack_launchpad_trigger_from_json,
		stack_launchpad_trigger_show_config_ui,
		stack_launchpad_trigger_config_to_json,
		stack_launchpad_trigger_config_free_json,
		stack_launchpad_trigger_config_from_json
	};
	stack_register_trigger_class(launchpad_trigger_class);
}

// The entry point for the plugin that Stack calls
extern "C" bool stack_init_plugin()
{
	stack_launchpad_trigger_register();
	return true;
}
