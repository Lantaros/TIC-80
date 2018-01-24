// MIT License

// Copyright (c) 2017 Vadim Grigoruk @nesbox // grigoruk@gmail.com

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "code.h"
#include "history.h"

#include <ctype.h>

#define TEXT_CURSOR_DELAY (TIC_FRAMERATE / 2)
#define TEXT_CURSOR_BLINK_PERIOD TIC_FRAMERATE
#define TEXT_BUFFER_WIDTH STUDIO_TEXT_BUFFER_WIDTH
#define TEXT_BUFFER_HEIGHT ((TIC80_HEIGHT - TOOLBAR_SIZE - STUDIO_TEXT_HEIGHT) / STUDIO_TEXT_HEIGHT)

struct OutlineItem
{
	char name[STUDIO_TEXT_BUFFER_WIDTH];
	char* pos;
};

#define OUTLINE_SIZE ((TIC80_HEIGHT - TOOLBAR_SIZE*2)/TIC_FONT_HEIGHT)
#define OUTLINE_ITEMS_SIZE (OUTLINE_SIZE * sizeof(OutlineItem))

static void history(Code* code)
{
	if(history_add(code->history))
		history_add(code->cursorHistory);
}

static void drawStatus(Code* code)
{
	const s32 Height = TIC_FONT_HEIGHT + 1;
	code->tic->api.rect(code->tic, 0, TIC80_HEIGHT - Height, TIC80_WIDTH, Height, (tic_color_white));
	code->tic->api.fixed_text(code->tic, code->status, 0, TIC80_HEIGHT - TIC_FONT_HEIGHT, getConfig()->theme.code.bg);
}

static void drawCursor(Code* code, s32 x, s32 y, char symbol)
{
	bool inverse = code->cursor.delay || code->tickCounter % TEXT_CURSOR_BLINK_PERIOD < TEXT_CURSOR_BLINK_PERIOD / 2;

	if(inverse)
	{
		code->tic->api.rect(code->tic, x-1, y-1, TIC_FONT_WIDTH+1, TIC_FONT_HEIGHT+1, getConfig()->theme.code.cursor);

		if(symbol)
			code->tic->api.draw_char(code->tic, symbol, x, y, getConfig()->theme.code.bg);
	}
}

static void drawCode(Code* code, bool withCursor)
{
	s32 xStart = code->rect.x - code->scroll.x * STUDIO_TEXT_WIDTH;
	s32 x = xStart;
	s32 y = code->rect.y - code->scroll.y * STUDIO_TEXT_HEIGHT;
	char* pointer = code->src;

	u8* colorPointer = code->colorBuffer;

	struct { char* start; char* end; } selection = {SDL_min(code->cursor.selection, code->cursor.position),
		SDL_max(code->cursor.selection, code->cursor.position)};

	struct { s32 x; s32 y; char symbol;	} cursor = {-1, -1, 0};

	while(*pointer)
	{
		char symbol = *pointer;

		if(x >= -TIC_FONT_WIDTH && x < TIC80_WIDTH && y >= -TIC_FONT_HEIGHT && y < TIC80_HEIGHT )
		{
			if(code->cursor.selection && pointer >= selection.start && pointer < selection.end)
				code->tic->api.rect(code->tic, x-1, y-1, TIC_FONT_WIDTH+1, TIC_FONT_HEIGHT+1, getConfig()->theme.code.select);
			else if(getConfig()->theme.code.shadow)
			{
				code->tic->api.draw_char(code->tic, symbol, x+1, y+1, 0);
			}

			code->tic->api.draw_char(code->tic, symbol, x, y, *colorPointer);			
		}

		if(code->cursor.position == pointer)
			cursor.x = x, cursor.y = y, cursor.symbol = symbol;

		if(symbol == '\n')
		{
			x = xStart;
			y += STUDIO_TEXT_HEIGHT;
		}
		else x += STUDIO_TEXT_WIDTH;

		pointer++;
		colorPointer++;
	}

	if(code->cursor.position == pointer)
		cursor.x = x, cursor.y = y;

	if(withCursor && cursor.x >= 0 && cursor.y >= 0)
		drawCursor(code, cursor.x, cursor.y, cursor.symbol);
}

static void getCursorPosition(Code* code, s32* x, s32* y)
{
	*x = 0;
	*y = 0;

	const char* pointer = code->src;

	while(*pointer)
	{
		if(code->cursor.position == pointer) return;

		if(*pointer == '\n')
		{
			*x = 0;
			(*y)++;
		}
		else (*x)++;

		pointer++;
	}
}

static s32 getLinesCount(Code* code)
{
	char* text = code->src;
	s32 count = 0;

	while(*text)
		if(*text++ == '\n')
			count++;

	return count;
}

static void removeInvalidChars(char* code)
{
	// remove \r symbol
	char* s; char* d;
	for(s = d = code; (*d = *s); d += (*s++ != '\r'));
}

static void updateEditor(Code* code)
{
	s32 column = 0;
	s32 line = 0;
	getCursorPosition(code, &column, &line);

	if(column < code->scroll.x) code->scroll.x = column;
	else if(column >= code->scroll.x + TEXT_BUFFER_WIDTH)
		code->scroll.x = column - TEXT_BUFFER_WIDTH + 1;

	if(line < code->scroll.y) code->scroll.y = line;
	else if(line >= code->scroll.y + TEXT_BUFFER_HEIGHT)
		code->scroll.y = line - TEXT_BUFFER_HEIGHT + 1;

	code->cursor.delay = TEXT_CURSOR_DELAY;

	// update status
	{
		memset(code->status, ' ', sizeof code->status - 1);

		char status[STUDIO_TEXT_BUFFER_WIDTH];
		s32 count = getLinesCount(code);
		sprintf(status, "line %i/%i col %i", line + 1, count + 1, column + 1);
		memcpy(code->status, status, strlen(status));

		size_t codeLen = strlen(code->src);
		sprintf(status, "%i/%i", (u32)codeLen, TIC_CODE_SIZE);

		memset(code->src + codeLen, '\0', TIC_CODE_SIZE - codeLen);
		memcpy(code->status + sizeof code->status - strlen(status) - 1, status, strlen(status));
	}
}

static inline bool isalnum_(char c) {return isalnum(c) || c == '_';}

static void parseSyntaxColor(Code* code)
{
	memset(code->colorBuffer, getConfig()->theme.code.syntax.var, sizeof(code->colorBuffer));

	tic_mem* tic = code->tic;

	const tic_script_config* config = tic->api.get_script_config(tic);

	if(config->parse)
		config->parse(config, code->src, code->colorBuffer, &getConfig()->theme.code.syntax);
}

static char* getLineByPos(Code* code, char* pos)
{
	char* text = code->src;
	char* line = text;

	while(text < pos)
		if(*text++ == '\n')
			line = text;

	return line;
}

static char* getLine(Code* code)
{
	return getLineByPos(code, code->cursor.position);
}

static char* getPrevLine(Code* code)
{
	char* text = code->src;
	char* pos = code->cursor.position;
	char* prevLine = text;
	char* line = text;

	while(text < pos)
		if(*text++ == '\n')
		{
			prevLine = line;
			line = text;
		}

	return prevLine;
}

static char* getNextLineByPos(Code* code, char* pos)
{
	while(*pos && *pos++ != '\n');

	return pos;
}

static char* getNextLine(Code* code)
{
	return getNextLineByPos(code, code->cursor.position);
}

static s32 getLineSize(const char* line)
{
	s32 size = 0;
	while(*line != '\n' && *line++) size++;

	return size;
}

static void updateColumn(Code* code)
{
	code->cursor.column = code->cursor.position - getLine(code);
}

static void updateCursorPosition(Code* code, char* position)
{
	code->cursor.position = position;
	updateColumn(code);
	updateEditor(code);
}

static void setCursorPosition(Code* code, s32 cx, s32 cy)
{
	s32 x = 0;
	s32 y = 0;
	char* pointer = code->src;

	while(*pointer)
	{
		if(y == cy && x == cx)
		{
			updateCursorPosition(code, pointer);
			return;
		}

		if(*pointer == '\n')
		{
			if(y == cy && cx > x)
			{
				updateCursorPosition(code, pointer);
				return;
			}

			x = 0;
			y++;
		}
		else x++;

		pointer++;
	}

	updateCursorPosition(code, pointer);
}

static void upLine(Code* code)
{
	char* prevLine = getPrevLine(code);
	size_t prevSize = getLineSize(prevLine);
	size_t size = code->cursor.column;

	code->cursor.position = prevLine + (prevSize > size ? size : prevSize);
}

static void downLine(Code* code)
{
	char* nextLine = getNextLine(code);
	size_t nextSize = getLineSize(nextLine);
	size_t size = code->cursor.column;

	code->cursor.position = nextLine + (nextSize > size ? size : nextSize);
}

static void leftColumn(Code* code)
{
	char* start = code->src;

	if(code->cursor.position > start)
	{
		code->cursor.position--;
		updateColumn(code);
	}
}

static void rightColumn(Code* code)
{
	if(*code->cursor.position)
	{
		code->cursor.position++;
		updateColumn(code);
	}
}

static void leftWord(Code* code)
{
	const char* start = code->src;
	char* pos = code->cursor.position-1;

	if(pos > start)
	{
		if(isalnum_(*pos)) while(pos > start && isalnum_(*(pos-1))) pos--;
		else while(pos > start && !isalnum_(*(pos-1))) pos--;

		code->cursor.position = pos;

		updateColumn(code);
	}
}

static void rightWord(Code* code)
{
	const char* end = code->src + strlen(code->src);
	char* pos = code->cursor.position;

	if(pos < end)
	{
		if(isalnum_(*pos)) while(pos < end && isalnum_(*pos)) pos++;
		else while(pos < end && !isalnum_(*pos)) pos++;

		code->cursor.position = pos;
		updateColumn(code);
	}
}

static void goHome(Code* code)
{
	code->cursor.position = getLine(code);

	updateColumn(code);
}

static void goEnd(Code* code)
{
	char* line = getLine(code);
	code->cursor.position = line + getLineSize(line);

	updateColumn(code);
}

static void goCodeHome(Code *code)
{
	code->cursor.position = code->src;

	updateColumn(code);
}

static void goCodeEnd(Code *code)
{
	code->cursor.position = code->src + strlen(code->src);

	updateColumn(code);
}

static void pageUp(Code* code)
{
	s32 column = 0;
	s32 line = 0;
	getCursorPosition(code, &column, &line);
	setCursorPosition(code, column, line > TEXT_BUFFER_HEIGHT ? line - TEXT_BUFFER_HEIGHT : 0);
}

static void pageDown(Code* code)
{
	s32 column = 0;
	s32 line = 0;
	getCursorPosition(code, &column, &line);
	s32 lines = getLinesCount(code);
	setCursorPosition(code, column, line < lines - TEXT_BUFFER_HEIGHT ? line + TEXT_BUFFER_HEIGHT : lines);
}

static bool replaceSelection(Code* code)
{
	char* pos = code->cursor.position;
	char* sel = code->cursor.selection;

	if(sel && sel != pos)
	{
		char* start = SDL_min(sel, pos);
		char* end = SDL_max(sel, pos);

		memmove(start, end, strlen(end) + 1);

		code->cursor.position = start;
		code->cursor.selection = NULL;

		history(code);

		parseSyntaxColor(code);

		return true;
	}

	return false;
}

static void deleteChar(Code* code)
{
	if(!replaceSelection(code))
	{
		char* pos = code->cursor.position;
		memmove(pos, pos + 1, strlen(pos));
		history(code);
		parseSyntaxColor(code);
	}
}

static void backspaceChar(Code* code)
{
	if(!replaceSelection(code) && code->cursor.position > code->src)
	{
		char* pos = --code->cursor.position;
		memmove(pos, pos + 1, strlen(pos));
		history(code);
		parseSyntaxColor(code);
	}
}

static void inputSymbolBase(Code* code, char sym)
{
	if (strlen(code->src) >= sizeof(tic_code))
		return;

	char* pos = code->cursor.position;

	memmove(pos + 1, pos, strlen(pos)+1);

	*code->cursor.position++ = sym;

	history(code);

	updateColumn(code);

	parseSyntaxColor(code);
}

static void inputSymbol(Code* code, char sym)
{
	replaceSelection(code);

	inputSymbolBase(code, sym);
}

static void newLine(Code* code)
{
	if(!replaceSelection(code))
	{
		char* ptr = getLine(code);
		size_t size = 0;

		while(*ptr == '\t' || *ptr == ' ') ptr++, size++;

		if(ptr > code->cursor.position)
			size -= ptr - code->cursor.position;

		inputSymbol(code, '\n');

		for(size_t i = 0; i < size; i++)
			inputSymbol(code, '\t');
	}
}

static void selectAll(Code* code)
{
	code->cursor.selection = code->src;
		code->cursor.position = code->cursor.selection + strlen(code->cursor.selection);
}

static void copyToClipboard(Code* code)
{
	char* pos = code->cursor.position;
	char* sel = code->cursor.selection;

	char* start = NULL;
	size_t size = 0;

	if(sel && sel != pos)
	{
		start = SDL_min(sel, pos);
		size = SDL_max(sel, pos) - start;
	}
	else
	{
		start = getLine(code);
		size = getNextLine(code) - start;
	}

	char* clipboard = (char*)SDL_malloc(size+1);

	if(clipboard)
	{
		memcpy(clipboard, start, size);
		clipboard[size] = '\0';
		SDL_SetClipboardText(clipboard);
		SDL_free(clipboard);
	}
}

static void cutToClipboard(Code* code)
{
	copyToClipboard(code);
	replaceSelection(code);
	history(code);
}

static void copyFromClipboard(Code* code)
{
	if(SDL_HasClipboardText())
	{
		char* clipboard = SDL_GetClipboardText();

		if(clipboard)
		{
			removeInvalidChars(clipboard);
			size_t size = strlen(clipboard);

			if(size)
			{
				replaceSelection(code);

				char* pos = code->cursor.position;

				// cut clipboard code if overall code > max code size
				{
					size_t codeSize = strlen(code->src);

					if (codeSize + size > sizeof(tic_code))
					{
						size = sizeof(tic_code) - codeSize;
						clipboard[size] = '\0';
					}
				}

				memmove(pos + size, pos, strlen(pos) + 1);
				memcpy(pos, clipboard, size);

				code->cursor.position += size;

				history(code);

				parseSyntaxColor(code);
			}

			SDL_free(clipboard);
		}
	}
}

static void update(Code* code)
{
	updateEditor(code);
	parseSyntaxColor(code);
}

static void undo(Code* code)
{
	history_undo(code->history);
	history_undo(code->cursorHistory);

	update(code);
}

static void redo(Code* code)
{
	history_redo(code->history);
	history_redo(code->cursorHistory);

	update(code);
}

static void doTab(Code* code, bool shift, bool crtl)
{
	char* cursor_position = code->cursor.position;
	char* cursor_selection = code->cursor.selection;
	
	bool has_selection = cursor_selection && cursor_selection != cursor_position;
	bool modifier_key_pressed = shift || crtl;
	
	if(has_selection || modifier_key_pressed)
	{
		char* start;
		char* end;
		
		bool changed = false;
		
		if(cursor_selection) {
			start = SDL_min(cursor_selection, cursor_position);
			end = SDL_max(cursor_selection, cursor_position);
		} else {
			start = end = cursor_position;
		}

		char* line = start = getLineByPos(code, start);

		while(line)
		{
			if(shift)
			{
				if(*line == '\t' || *line == ' ')
				{
					memmove(line, line + 1, strlen(line)+1);
					end--;
					changed = true;
				}
			}
			else
			{
				memmove(line + 1, line, strlen(line)+1);
				*line = '\t';
				end++;

				changed = true;
			}
			
			line = getNextLineByPos(code, line);
			if(line >= end) break;
		}
		
		if(changed) {
			
			if(has_selection) {
				code->cursor.position = start;
				code->cursor.selection = end;
			}
			else if (start <= end) code->cursor.position = end;
			
			history(code);
			parseSyntaxColor(code);
		}
	}
	else inputSymbolBase(code, '\t');
}

static void setFindMode(Code* code)
{
	if(code->cursor.selection)
	{
		const char* end = SDL_max(code->cursor.position, code->cursor.selection);
		const char* start = SDL_min(code->cursor.position, code->cursor.selection);
		size_t len = end - start;

		if(len > 0 && len < sizeof code->popup.text - 1)
		{
			memset(code->popup.text, 0, sizeof code->popup.text);
			memcpy(code->popup.text, start, len);
		}
	}
}

static void setGotoMode(Code* code)
{
	code->jump.line = -1;
}

static int funcCompare(const void* a, const void* b)
{
	const OutlineItem* item1 = (const OutlineItem*)a;
	const OutlineItem* item2 = (const OutlineItem*)b;

	if(item1->pos == NULL) return 1;
	if(item2->pos == NULL) return -1;

	return SDL_strcasecmp(item1->name, item2->name);
}

static void normalizeScroll(Code* code)
{
	if(code->scroll.x < 0) code->scroll.x = 0;
	if(code->scroll.y < 0) code->scroll.y = 0;
	else
	{
		s32 lines = getLinesCount(code);
		if(code->scroll.y > lines) code->scroll.y = lines;
	}
}

static void centerScroll(Code* code)
{
	s32 col, line;
	getCursorPosition(code, &col, &line);
	code->scroll.x = col - TEXT_BUFFER_WIDTH / 2;
	code->scroll.y = line - TEXT_BUFFER_HEIGHT / 2;

	normalizeScroll(code);
}

static void updateOutlineCode(Code* code)
{
	OutlineItem* item = code->outline.items + code->outline.index;

	if(item->pos)
	{
		code->cursor.position = item->pos;
		code->cursor.selection = item->pos + strlen(item->name);
	}
	else
	{
		code->cursor.position = code->src;
		code->cursor.selection = NULL;
	}

	centerScroll(code);
	updateEditor(code);
}

static void initOutlineMode(Code* code)
{
	OutlineItem* out = code->outline.items;
	OutlineItem* end = out + OUTLINE_SIZE;

	tic_mem* tic = code->tic;

	char buffer[STUDIO_TEXT_BUFFER_WIDTH] = {0};
	char filter[STUDIO_TEXT_BUFFER_WIDTH] = {0};
	SDL_strlcpy(filter, code->popup.text, sizeof(filter));
	SDL_strlwr(filter);

	const tic_script_config* config = tic->api.get_script_config(tic);

	if(config->getOutline)
	{
		s32 size = 0;
		const tic_outline_item* items = config->getOutline(code->src, &size);

		for(s32 i = 0; i < size; i++)
		{
			const tic_outline_item* item = items + i;

			if(out < end)
			{
				out->pos = code->src + item->pos;
				memset(out->name, 0, STUDIO_TEXT_BUFFER_WIDTH);
				memcpy(out->name, out->pos, SDL_min(item->size, STUDIO_TEXT_BUFFER_WIDTH-1));

				if(*filter)
				{
					SDL_strlcpy(buffer, out->name, sizeof(buffer));
					SDL_strlwr(buffer);

					if(strstr(buffer, filter)) out++;
					else out->pos = NULL;
				}
				else out++;
			}
			else break;
		}
	}
}

static void setOutlineMode(Code* code)
{
	code->outline.index = 0;
	memset(code->outline.items, 0, OUTLINE_ITEMS_SIZE);

	initOutlineMode(code);

	qsort(code->outline.items, OUTLINE_SIZE, sizeof(OutlineItem), funcCompare);
	updateOutlineCode(code);
}

static void setCodeMode(Code* code, s32 mode)
{
	if(code->mode != mode)
	{
		strcpy(code->popup.text, "");

		code->popup.prevPos = code->cursor.position;
		code->popup.prevSel = code->cursor.selection;

		switch(mode)
		{
		case TEXT_FIND_MODE: setFindMode(code); break;
		case TEXT_GOTO_MODE: setGotoMode(code); break;
		case TEXT_OUTLINE_MODE: setOutlineMode(code); break;
		default: break;
		}

		code->mode = mode;
	}
}

static void commentLine(Code* code)
{
	const char* comment = code->tic->api.get_script_config(code->tic)->singleComment;
	size_t size = strlen(comment);

	char* line = getLine(code);

	const char* end = line + getLineSize(line);

	while((*line == ' ' || *line == '\t') && line < end) line++;

	if(memcmp(line, comment, size))
	{
		if (strlen(code->src) + size >= sizeof(tic_code))
			return;

		memmove(line + size, line, strlen(line)+1);
		memcpy(line, comment, size);

		if(code->cursor.position > line)
			code->cursor.position += size;
	}
	else
	{
		memmove(line, line + size, strlen(line + size)+1);

		if(code->cursor.position > line + size)
			code->cursor.position -= size;
	}

	code->cursor.selection = NULL;	

	history(code);

	parseSyntaxColor(code);
}

static void processKeydown(Code* code, SDL_Keycode keycode)
{
	switch(keycode)
	{
	case SDLK_LCTRL:
	case SDLK_RCTRL:
	case SDLK_LSHIFT:
	case SDLK_RSHIFT:
	case SDLK_LALT:
	case SDLK_RALT:
		return;
	}

	switch(getClipboardEvent(keycode))
	{
	case TIC_CLIPBOARD_CUT: cutToClipboard(code); break;
	case TIC_CLIPBOARD_COPY: copyToClipboard(code); break;
	case TIC_CLIPBOARD_PASTE: copyFromClipboard(code); break;
	default: break;
	}

	SDL_Keymod keymod = SDL_GetModState();

	switch(keycode)
	{
	case SDLK_UP:
	case SDLK_DOWN:
	case SDLK_LEFT:
	case SDLK_RIGHT:
	case SDLK_HOME:
	case SDLK_END:
	case SDLK_PAGEUP:
	case SDLK_PAGEDOWN:

		if(!(keymod & KMOD_SHIFT)) code->cursor.selection = NULL;
		else if(code->cursor.selection == NULL) code->cursor.selection = code->cursor.position;
	}

	if(keymod & TIC_MOD_CTRL)
	{
		if(keymod & KMOD_CTRL)
		{
			switch(keycode)
			{
			case SDLK_LEFT: 	leftWord(code); break;
			case SDLK_RIGHT: 	rightWord(code); break;
			case SDLK_TAB:		doTab(code, keymod & KMOD_SHIFT, keymod & KMOD_CTRL); break;
			}
		}
		else if(keymod & KMOD_GUI)
		{
			switch(keycode)
			{
			case SDLK_LEFT: 	goHome(code); break;
			case SDLK_RIGHT: 	goEnd(code); break;
			}
		}

		switch(keycode)
		{
		case SDLK_a:	selectAll(code); break;
		case SDLK_z: 	undo(code); break;
		case SDLK_y: 	redo(code); break;
		case SDLK_f: 	setCodeMode(code, TEXT_FIND_MODE); break;
		case SDLK_g: 	setCodeMode(code, TEXT_GOTO_MODE); break;
		case SDLK_o: 	setCodeMode(code, TEXT_OUTLINE_MODE); break;
		case SDLK_SLASH: commentLine(code);	break;
		case SDLK_HOME: goCodeHome(code); break;
		case SDLK_END: 	goCodeEnd(code); break;
		}
	}
	else if(keymod & KMOD_ALT)
	{
		switch(keycode)
		{
		case SDLK_LEFT: 	leftWord(code); break;
		case SDLK_RIGHT: 	rightWord(code); break;
		}
	}
	else
	{
		switch(keycode)
		{
		case SDLK_UP: 			upLine(code); break;
		case SDLK_DOWN: 		downLine(code); break;
		case SDLK_LEFT: 		leftColumn(code); break;
		case SDLK_RIGHT: 		rightColumn(code); break;
		case SDLK_HOME: 		goHome(code); break;
		case SDLK_END: 			goEnd(code); break;
		case SDLK_PAGEUP: 		pageUp(code); break;
		case SDLK_PAGEDOWN: 	pageDown(code); break;
		case SDLK_DELETE: 		deleteChar(code); break;
		case SDLK_BACKSPACE: 	backspaceChar(code); break;
		case SDLK_RETURN: 		newLine(code); break;
		case SDLK_TAB: 			doTab(code, keymod & KMOD_SHIFT, keymod & KMOD_CTRL); break;
		}
	}

	updateEditor(code);
}

static void processGestures(Code* code)
{
	SDL_Point point = {0, 0};

	if(getGesturePos(&point))
	{
		if(code->scroll.gesture)
		{
			code->scroll.x = (code->scroll.start.x - point.x) / STUDIO_TEXT_WIDTH;
			code->scroll.y = (code->scroll.start.y - point.y) / STUDIO_TEXT_HEIGHT;

			normalizeScroll(code);
		}
		else
		{
			code->scroll.start.x = point.x + code->scroll.x * STUDIO_TEXT_WIDTH;
			code->scroll.start.y = point.y + code->scroll.y * STUDIO_TEXT_HEIGHT;
			code->scroll.gesture = true;
		}
	}
	else code->scroll.gesture = false;
}

static void processMouse(Code* code)
{
	if(checkMousePos(&code->rect))
	{
		setCursor(SDL_SYSTEM_CURSOR_IBEAM);

		if(code->scroll.active)
		{
			if(checkMouseDown(&code->rect, SDL_BUTTON_RIGHT))
			{
				code->scroll.x = (code->scroll.start.x - getMouseX()) / STUDIO_TEXT_WIDTH;
				code->scroll.y = (code->scroll.start.y - getMouseY()) / STUDIO_TEXT_HEIGHT;

				normalizeScroll(code);
			}
			else code->scroll.active = false;
		}
		else
		{
			if(checkMouseDown(&code->rect, SDL_BUTTON_LEFT))
			{
				s32 mx = getMouseX();
				s32 my = getMouseY();

				s32 x = (mx - code->rect.x) / STUDIO_TEXT_WIDTH;
				s32 y = (my - code->rect.y) / STUDIO_TEXT_HEIGHT;

				char* position = code->cursor.position;
				setCursorPosition(code, x + code->scroll.x, y + code->scroll.y);

				if(SDL_GetModState() & KMOD_SHIFT)
				{
					code->cursor.selection = code->cursor.position;
					code->cursor.position = position;
				}
				else if(!code->cursor.mouseDownPosition)
				{
					code->cursor.selection = code->cursor.position;
					code->cursor.mouseDownPosition = code->cursor.position;
				}
			}
			else
			{
				if(code->cursor.mouseDownPosition == code->cursor.position)
					code->cursor.selection = NULL;

				code->cursor.mouseDownPosition = NULL;
			}

			if(checkMouseDown(&code->rect, SDL_BUTTON_RIGHT))
			{
				code->scroll.active = true;

				code->scroll.start.x = getMouseX() + code->scroll.x * STUDIO_TEXT_WIDTH;
				code->scroll.start.y = getMouseY() + code->scroll.y * STUDIO_TEXT_HEIGHT;
			}

		}
	}
}

static void textEditTick(Code* code)
{
// 	SDL_Event* event = NULL;
// 	while ((event = pollEvent()))
// 	{
// 		switch(event->type)
// 		{
// 		case SDL_MOUSEWHEEL:
// 			{
// 				enum{Scroll = 3};
// 				s32 delta = event->wheel.y > 0 ? -Scroll : Scroll;

// 				code->scroll.y += delta;

// 				normalizeScroll(code);
// 			}
// 			break;
// 		case SDL_KEYDOWN:
// 			processKeydown(code, event->key.keysym.sym);
// 			break;
// 		case SDL_TEXTINPUT:

// #if defined(__LINUX__)
// 			if(!(SDL_GetModState() & KMOD_LALT))
// #endif

// 			if(strlen(event->text.text) == 1)
// 			{


// 				inputSymbol(code, *event->text.text);
// 				updateEditor(code);
// 			}
// 			break;
// 		}
// 	}

	processGestures(code);
	processMouse(code);

	code->tic->api.clear(code->tic, getConfig()->theme.code.bg);

	drawCode(code, true);
	drawStatus(code);
}

static void drawPopupBar(Code* code, const char* title)
{
	enum {TextY = TOOLBAR_SIZE + 1};

	code->tic->api.rect(code->tic, 0, TOOLBAR_SIZE, TIC80_WIDTH, TIC_FONT_HEIGHT + 1, (tic_color_blue));
	code->tic->api.fixed_text(code->tic, title, 0, TextY, (tic_color_white));

	code->tic->api.fixed_text(code->tic, code->popup.text, (s32)strlen(title)*TIC_FONT_WIDTH, TextY, (tic_color_white));

	drawCursor(code, (s32)(strlen(title) + strlen(code->popup.text)) * TIC_FONT_WIDTH, TextY, ' ');
}

static void updateFindCode(Code* code, char* pos)
{
	if(pos)
	{
		code->cursor.position = pos;
		code->cursor.selection = pos + strlen(code->popup.text);

		centerScroll(code);
		updateEditor(code);
	}
}

static char* upStrStr(const char* start, const char* from, const char* substr)
{
	const char* ptr = from-1;
	size_t len = strlen(substr);

	if(len > 0)
	{
		while(ptr >= start)
		{
			if(memcmp(ptr, substr, len) == 0)
				return (char*)ptr;

			ptr--;
		}
	}

	return NULL;
}

static char* downStrStr(const char* start, const char* from, const char* substr)
{
	return strstr(from, substr);
}

static void textFindTick(Code* code)
{
	// SDL_Event* event = NULL;
	// while ((event = pollEvent()))
	// {
	// 	switch(event->type)
	// 	{
	// 	case SDL_KEYDOWN:
	// 		switch(event->key.keysym.sym)
	// 		{
	// 		case SDLK_RETURN:
	// 			setCodeMode(code, TEXT_EDIT_MODE);
	// 			break;
	// 		case SDLK_UP:
	// 		case SDLK_LEFT:
	// 		case SDLK_DOWN:
	// 		case SDLK_RIGHT:
	// 			if(*code->popup.text)
	// 			{
	// 				SDL_Keycode keycode = event->key.keysym.sym;
	// 				bool reverse = keycode == SDLK_UP || keycode == SDLK_LEFT;
	// 				char* (*func)(const char*, const char*, const char*) = reverse ? upStrStr : downStrStr;
	// 				char* from = reverse ? SDL_min(code->cursor.position, code->cursor.selection) : SDL_max(code->cursor.position, code->cursor.selection);
	// 				char* pos = func(code->src, from, code->popup.text);
	// 				updateFindCode(code, pos);
	// 			}
	// 			break;
	// 		case SDLK_BACKSPACE:
	// 			if(*code->popup.text)
	// 			{
	// 				code->popup.text[strlen(code->popup.text)-1] = '\0';
	// 				updateFindCode(code, strstr(code->src, code->popup.text));
	// 			}
	// 			break;
	// 		default: break;
	// 		}
	// 		break;
	// 	case SDL_TEXTINPUT:
	// 		if(strlen(event->text.text) == 1)
	// 		{
	// 			if(strlen(code->popup.text) + 1 < sizeof code->popup.text)
	// 			{
	// 				strcat(code->popup.text, event->text.text);
	// 				updateFindCode(code, strstr(code->src, code->popup.text));
	// 			}
	// 		}
	// 		break;
	// 	default: break;
	// 	}
	// }

	code->tic->api.clear(code->tic, getConfig()->theme.code.bg);

	drawCode(code, false);
	drawPopupBar(code, " FIND:");
	drawStatus(code);
}

static void updateGotoCode(Code* code)
{
	s32 line = atoi(code->popup.text);

	if(line) line--;

	s32 count = getLinesCount(code);

	if(line > count) line = count;

	code->cursor.selection = NULL;
	setCursorPosition(code, 0, line);

	code->jump.line = line;

	centerScroll(code);
	updateEditor(code);
}

static void textGoToTick(Code* code)
{
	// SDL_Event* event = NULL;
	// while ((event = pollEvent()))
	// {
	// 	switch(event->type)
	// 	{
	// 	case SDL_KEYDOWN:
	// 		switch(event->key.keysym.sym)
	// 		{
	// 		case SDLK_RETURN:
	// 			if(*code->popup.text)
	// 				updateGotoCode(code);

	// 			setCodeMode(code, TEXT_EDIT_MODE);
	// 			break;
	// 		case SDLK_BACKSPACE:
	// 			if(*code->popup.text)
	// 			{
	// 				code->popup.text[strlen(code->popup.text)-1] = '\0';
	// 				updateGotoCode(code);
	// 			}
	// 			break;
	// 		default: break;
	// 		}
	// 		break;
	// 	case SDL_TEXTINPUT:
	// 		if(strlen(event->text.text) == 1)
	// 		{
	// 			char sym = *event->text.text;

	// 			if(strlen(code->popup.text)+1 < sizeof code->popup.text && sym >= '0' && sym <= '9')
	// 			{
	// 				strcat(code->popup.text, event->text.text);
	// 				updateGotoCode(code);
	// 			}
	// 		}
	// 		break;
	// 	default: break;
	// 	}
	// }

	code->tic->api.clear(code->tic, getConfig()->theme.code.bg);

	if(code->jump.line >= 0)
		code->tic->api.rect(code->tic, 0, (code->jump.line - code->scroll.y) * TIC_FONT_HEIGHT + TOOLBAR_SIZE + 1,
			TIC80_WIDTH, TIC_FONT_HEIGHT+1, getConfig()->theme.code.select);

	drawCode(code, false);
	drawPopupBar(code, " GOTO:");
	drawStatus(code);
}

static void drawOutlineBar(Code* code, s32 x, s32 y)
{
	SDL_Rect rect = {x, y, TIC80_WIDTH - x, TIC80_HEIGHT - y};

	if(checkMousePos(&rect))
	{
		s32 mx = getMouseY() - rect.y;
		mx /= STUDIO_TEXT_HEIGHT;

		if(mx < OUTLINE_SIZE && code->outline.items[mx].pos)
		{
			setCursor(SDL_SYSTEM_CURSOR_HAND);

			if(checkMouseDown(&rect, SDL_BUTTON_LEFT))
			{
				code->outline.index = mx;
				updateOutlineCode(code);

			}

			if(checkMouseClick(&rect, SDL_BUTTON_LEFT))
				setCodeMode(code, TEXT_EDIT_MODE);
		}
	}

	code->tic->api.rect(code->tic, rect.x-1, rect.y, rect.w+1, rect.h, (tic_color_blue));

	OutlineItem* ptr = code->outline.items;

	y++;

	if(ptr->pos)
	{
		code->tic->api.rect(code->tic, rect.x - 1, rect.y + code->outline.index*STUDIO_TEXT_HEIGHT,
			rect.w + 1, TIC_FONT_HEIGHT + 1, (tic_color_red));
		while(ptr->pos)
		{
			code->tic->api.fixed_text(code->tic, ptr->name, x, y, (tic_color_white));
			ptr++;
			y += STUDIO_TEXT_HEIGHT;
		}
	}
	else code->tic->api.fixed_text(code->tic, "(empty)", x, y, (tic_color_white));
}

static void textOutlineTick(Code* code)
{
	// SDL_Event* event = NULL;
	// while ((event = pollEvent()))
	// {
	// 	switch(event->type)
	// 	{
	// 	case SDL_KEYDOWN:
	// 		switch(event->key.keysym.sym)
	// 		{
	// 		case SDLK_UP:
	// 			if(code->outline.index > 0)
	// 			{
	// 				code->outline.index--;
	// 				updateOutlineCode(code);
	// 			}
	// 			break;
	// 		case SDLK_DOWN:
	// 			if(code->outline.index < OUTLINE_SIZE - 1 && code->outline.items[code->outline.index + 1].pos)
	// 			{
	// 				code->outline.index++;
	// 				updateOutlineCode(code);
	// 			}
	// 			break;
	// 		case SDLK_RETURN:
	// 			updateOutlineCode(code);
	// 			setCodeMode(code, TEXT_EDIT_MODE);
	// 			break;
	// 		case SDLK_BACKSPACE:
	// 			if(*code->popup.text)
	// 			{
	// 				code->popup.text[strlen(code->popup.text)-1] = '\0';
	// 				setOutlineMode(code);
	// 			}
	// 			break;
	// 		default: break;
	// 		}
	// 		break;
	// 	case SDL_TEXTINPUT:
	// 		if(strlen(event->text.text) == 1)
	// 		{
	// 			if(strlen(code->popup.text) + 1 < sizeof code->popup.text)
	// 			{
	// 				strcat(code->popup.text, event->text.text);
	// 				setOutlineMode(code);
	// 			}
	// 		}
	// 		break;
	// 	default: break;
	// 	}
	// }

	code->tic->api.clear(code->tic, getConfig()->theme.code.bg);

	drawCode(code, false);
	drawPopupBar(code, " FUNC:");
	drawStatus(code);
	drawOutlineBar(code, TIC80_WIDTH - 12 * TIC_FONT_WIDTH, 2*(TIC_FONT_HEIGHT+1));
}

static void drawCodeToolbar(Code* code)
{
	code->tic->api.rect(code->tic, 0, 0, TIC80_WIDTH, TOOLBAR_SIZE, (tic_color_white));

	static const u8 Icons[] =
	{
		0b00000000,
		0b00100000,
		0b00110000,
		0b00111000,
		0b00110000,
		0b00100000,
		0b00000000,
		0b00000000,

		0b00000000,
		0b00111000,
		0b01000100,
		0b00111000,
		0b00010000,
		0b00010000,
		0b00000000,
		0b00000000,

		0b00000000,
		0b00010000,
		0b00011000,
		0b01111100,
		0b00011000,
		0b00010000,
		0b00000000,
		0b00000000,

		0b00000000,
		0b01111100,
		0b00000000,
		0b01111100,
		0b00000000,
		0b01111100,
		0b00000000,
		0b00000000,
	};

	enum {Count = sizeof Icons / BITS_IN_BYTE};
	enum {Size = 7};

	static const char* Tips[] = {"RUN [ctrl+r]","FIND [ctrl+f]", "GOTO [ctrl+g]", "OUTLINE [ctrl+o]"};

	for(s32 i = 0; i < Count; i++)
	{
		SDL_Rect rect = {TIC80_WIDTH + (i - Count) * Size, 0, Size, Size};

		bool over = false;
		if(checkMousePos(&rect))
		{
			setCursor(SDL_SYSTEM_CURSOR_HAND);

			showTooltip(Tips[i]);

			over = true;

			if(checkMouseClick(&rect, SDL_BUTTON_LEFT))
			{
				if (i == TEXT_RUN_CODE)
				{
					runProject();
				}
				else
				{
					s32 mode = TEXT_EDIT_MODE + i;

					if(code->mode == mode) code->escape(code);
					else setCodeMode(code, mode);
				}
			}
		}

		bool active = i == code->mode - TEXT_EDIT_MODE  && i != 0;
		if(active)
			code->tic->api.rect(code->tic, rect.x, rect.y, Size, Size, (tic_color_blue));

		drawBitIcon(rect.x, rect.y, Icons + i*BITS_IN_BYTE, active ? (tic_color_white) : (over ? (tic_color_dark_gray) : (tic_color_light_blue)));
	}

	drawToolbar(code->tic, getConfig()->theme.code.bg, false);
}

static void tick(Code* code)
{
	if(code->cursor.delay)
		code->cursor.delay--;

	code->cursor.tick++;

	switch(code->mode)
	{
	case TEXT_RUN_CODE: runProject(); break;
	case TEXT_EDIT_MODE: textEditTick(code); break;
	case TEXT_FIND_MODE: textFindTick(code); break;
	case TEXT_GOTO_MODE: textGoToTick(code); break;
	case TEXT_OUTLINE_MODE: textOutlineTick(code); break;
	}

	drawCodeToolbar(code);

	code->tickCounter++;
}

static void escape(Code* code)
{
	if(code->mode != TEXT_EDIT_MODE)
	{
		code->cursor.position = code->popup.prevPos;
		code->cursor.selection = code->popup.prevSel;
		code->popup.prevSel = code->popup.prevPos = NULL;

		code->mode = TEXT_EDIT_MODE;

		updateEditor(code);
	}
}

static void onStudioEvent(Code* code, StudioEvent event)
{
	switch(event)
	{
	case TIC_TOOLBAR_CUT: cutToClipboard(code); break;
	case TIC_TOOLBAR_COPY: copyToClipboard(code); break;
	case TIC_TOOLBAR_PASTE: copyFromClipboard(code); break;
	case TIC_TOOLBAR_UNDO: undo(code); break;
	case TIC_TOOLBAR_REDO: redo(code); break;
	}
}

void initCode(Code* code, tic_mem* tic, tic_code* src)
{
	if(code->outline.items == NULL)
		code->outline.items = (OutlineItem*)SDL_malloc(OUTLINE_ITEMS_SIZE);

	if(code->history) history_delete(code->history);
	if(code->cursorHistory) history_delete(code->cursorHistory);

	*code = (Code)
	{
		.tic = tic,
		.src = src->data,
		.tick = tick,
		.escape = escape,
		.cursor = {{src->data, NULL, 0, 0}, NULL, 0},
		.rect = {0, TOOLBAR_SIZE + 1, TIC80_WIDTH, TIC80_HEIGHT - TOOLBAR_SIZE - TIC_FONT_HEIGHT - 1},
		.scroll = {0, 0, {0, 0}, false},
		.tickCounter = 0,
		.history = NULL,
		.cursorHistory = NULL,
		.mode = TEXT_EDIT_MODE,
		.jump = {.line = -1},
		.popup =
		{
			.prevPos = NULL,
			.prevSel = NULL,
		},
		.outline =
		{
			.items = code->outline.items,
			.index = 0,
		},
		.event = onStudioEvent,
		.update = update,
	};

	code->history = history_create(code->src, sizeof(tic_code));
	code->cursorHistory = history_create(&code->cursor, sizeof code->cursor);

	update(code);
}
