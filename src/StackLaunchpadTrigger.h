#ifndef STACKLAUNCHPADTRIGGER_H_INCLUDED
#define STACKLAUNCHPADTRIGGER_H_INCLUDED

// Includes:
#include "StackTrigger.h"

typedef struct StackLaunchpadTrigger
{
	// Superclass
	StackTrigger super;

	// The description for the trigger
	char *description;

	uint8_t row;
	uint8_t column;
	uint8_t r;
	uint8_t g;
	uint8_t b;
	bool on_pressed;

	// Enable for Cue List Controls
	bool use_for_cue_list;

	// Buffer for our event text
	char event_text[48];
} StackKeyTrigger;

// Defines:
#define STACK_LAUNCHPAD_TRIGGER(_t) ((StackLaunchpadTrigger*)(_t))

#endif
