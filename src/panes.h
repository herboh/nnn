/* Parent listing and cached previews for the three-pane layout. */

#define PANE_PREVIEW_BYTES (128 * 1024)

static struct {
	char path[PATH_MAX];
	struct stat info;
	struct dirent **entries;
	int count;
} parent_pane;

static struct {
	char path[PATH_MAX];
	struct stat info;
	int width, height;
	bool showhidden;
	size_t length;
	unsigned char data[PANE_PREVIEW_BYTES + 1];
} file_preview;

static bool same_pane_file(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino
	       && a->st_size == b->st_size && a->st_mtime == b->st_mtime
	       && a->st_ctime == b->st_ctime && NSEC_MTIME(*a) == NSEC_MTIME(*b)
	       && NSEC_CTIME(*a) == NSEC_CTIME(*b);
}

static int pane_text(const char *text, size_t length, int columns)
{
#ifndef NOLC
	mbstate_t state = {0};
#endif
	while (length && columns > 0) {
#ifdef NOLC
		unsigned char ch = (unsigned char)*text;
		addch(ch == '\t' ? ' ' : (ch < 32 || ch == 127 ? '?' : ch));
		++text;
		--length;
		--columns;
#else
		wchar_t ch;
		size_t bytes = mbrtowc(&ch, text, length, &state);
		if (!bytes || bytes == (size_t)-1 || bytes == (size_t)-2) {
			ch = L'?';
			bytes = 1;
			memset(&state, 0, sizeof(state));
		}
		int width = wcwidth(ch);
		if (width < 0) {
			ch = ch == L'\t' ? L' ' : L'?';
			width = 1;
		}
		if (width > columns)
			return 0;
		addnwstr(&ch, 1);
		text += bytes;
		length -= bytes;
		columns -= width;
#endif
	}
	return columns;
}

static void pane_title(int column, int width, const char *title)
{
	move(0, column);
	attron(A_BOLD | COLOR_PAIR(cfg.curctx + 1));
	pane_text(title, strlen(title), width);
	attroff(A_BOLD | COLOR_PAIR(cfg.curctx + 1));
}

static void pane_border(int column)
{
	mvvline(0, column, ACS_VLINE | A_DIM | COLOR_PAIR(cfg.curctx + 1), xlines - 1);
}

static int pane_entry(const struct dirent *entry)
{
	return strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..");
}

static int pane_entry_order(const struct dirent **a, const struct dirent **b)
{
	int directory_order = ((*b)->d_type == DT_DIR) - ((*a)->d_type == DT_DIR);
	return directory_order ? directory_order : xstrverscasecmp((*a)->d_name, (*b)->d_name);
}

static void free_parent_pane(void)
{
	for (int i = 0; i < parent_pane.count; ++i)
		free(parent_pane.entries[i]);
	free(parent_pane.entries);
	parent_pane.entries = NULL;
	parent_pane.count = 0;
	parent_pane.path[0] = '\0';
}

static void draw_parent_pane(char *path)
{
	char parent[PATH_MAX];
	const char *current = xbasename(path);
	xstrsncpy(parent, path, sizeof(parent));
	xdirname(parent);

	pane_border(list_left - 1);
	pane_title(1, list_left - 3, istopdir(parent) ? "/" : xbasename(parent));

	struct stat info;
	if (stat(parent, &info) == -1)
		return;
	if (strcmp(parent_pane.path, parent) || !same_pane_file(&parent_pane.info, &info)) {
		free_parent_pane();
		struct dirent **entries = NULL;
		int count = scandir(parent, &entries, pane_entry, pane_entry_order);
		if (count < 0)
			return;
		parent_pane.entries = entries;
		parent_pane.count = count;
		parent_pane.info = info;
		xstrsncpy(parent_pane.path, parent, sizeof(parent_pane.path));
	}

	int rows = xlines - 3, visible = 0, current_index = 0;
	for (int i = 0; i < parent_pane.count; ++i) {
		const char *name = parent_pane.entries[i]->d_name;
		if (!strcmp(name, current))
			current_index = visible;
		else if (!cfg.showhidden && name[0] == '.')
			continue;
		++visible;
	}

	int first = MAX(0, MIN(current_index - rows / 2, visible - rows));
	for (int i = 0, row = 2; i < parent_pane.count && row < xlines - 1; ++i) {
		const struct dirent *entry = parent_pane.entries[i];
		bool active = !strcmp(entry->d_name, current);
		if ((!active && !cfg.showhidden && entry->d_name[0] == '.') || first-- > 0)
			continue;
		int attrs = active ? A_REVERSE | A_BOLD : A_DIM;
		if (entry->d_type == DT_DIR || active)
			attrs |= COLOR_PAIR(C_DIR);
		move(row++, 1);
		attron(attrs);
#ifdef ICONS_ENABLED
		addstr(entry->d_type == DT_DIR || active ? "\uf07b " : "\uf15b ");
#else
		addstr(entry->d_type == DT_DIR || active ? "/ " : "  ");
#endif
		pane_text(entry->d_name, strlen(entry->d_name), list_left - 5);
		attroff(attrs);
	}
}

static int pane_cube(unsigned char value)
{
	return value < 48 ? 0 : (value < 115 ? 1 : (value - 35) / 40);
}

static int pane_rgb(int r, int g, int b)
{
	static const int cube[] = {0, 95, 135, 175, 215, 255};
	int x = pane_cube(r), y = pane_cube(g), z = pane_cube(b);
	int gray = MIN(23, MAX(0, ((r + g + b) / 3 - 3) / 10));
	int level = 8 + 10 * gray;
	int dx = r - cube[x], dy = g - cube[y], dz = b - cube[z];
	int dr = r - level, dg = g - level, db = b - level;
	if (dr * dr + dg * dg + db * db < dx * dx + dy * dy + dz * dz)
		return 232 + gray;
	return 16 + 36 * x + 6 * y + z;
}

static void pane_color(attr_t attrs, int fg, int bg)
{
	static ushort_t pairs[257][257];
	static int next = C_PANE_PREVIEW;
	int pair = 0;

	if ((fg >= 0 || bg >= 0) && fg < COLORS && bg < COLORS) {
		ushort_t *cached = &pairs[fg + 1][bg + 1];
		if (!*cached && next < COLOR_PAIRS && next <= UINT16_MAX) {
#ifdef NCURSES_EXT_COLORS
			if (init_extended_pair(next, fg, bg) == OK)
#else
			if (next <= SHRT_MAX && init_pair(next, fg, bg) == OK)
#endif
				*cached = next++;
		}
		pair = *cached;
	}
#ifdef NCURSES_EXT_COLORS
	attr_set(attrs, 0, &pair);
#else
	attr_set(attrs, pair, NULL);
#endif
}

struct pane_style {
	attr_t attrs;
	int fg, bg;
};

static void pane_sgr(struct pane_style *style, const char *p, const char *end)
{
	int codes[24] = {0}, count = 1;
	for (; p < end; ++p) {
		if (*p == ';' && count < (int)ELEMENTS(codes))
			++count;
		else if (xisdigit(*p) && codes[count - 1] < 100)
			codes[count - 1] = 10 * codes[count - 1] + (*p - '0');
		else
			return;
	}

	for (int i = 0; i < count; ++i) {
		int code = codes[i];
		if (!code)
			*style = (struct pane_style){A_NORMAL, -1, -1};
		else if (code == 1)
			style->attrs |= A_BOLD;
		else if (code == 2)
			style->attrs |= A_DIM;
		else if (code == 3)
			style->attrs |= A_ITALIC;
		else if (code == 4)
			style->attrs |= A_UNDERLINE;
		else if (code == 22)
			style->attrs &= ~(A_BOLD | A_DIM);
		else if (code == 23)
			style->attrs &= ~A_ITALIC;
		else if (code == 24)
			style->attrs &= ~A_UNDERLINE;
		else if (code == 39 || code == 49)
			*(code == 39 ? &style->fg : &style->bg) = -1;
		else if (code >= 30 && code <= 37)
			style->fg = code - 30;
		else if (code >= 40 && code <= 47)
			style->bg = code - 40;
		else if (code >= 90 && code <= 97)
			style->fg = code - 82;
		else if (code >= 100 && code <= 107)
			style->bg = code - 92;
		else if ((code == 38 || code == 48) && i + 2 < count) {
			int *color = code == 38 ? &style->fg : &style->bg;
			if (codes[i + 1] == 5 && codes[i + 2] <= 255) {
				*color = codes[i + 2];
				i += 2;
			} else if (codes[i + 1] == 2 && i + 4 < count
				   && codes[i + 2] <= 255 && codes[i + 3] <= 255 && codes[i + 4] <= 255) {
				*color = pane_rgb(codes[i + 2], codes[i + 3], codes[i + 4]);
				i += 4;
			}
		}
	}
	pane_color(style->attrs, style->fg, style->bg);
}

static void pane_markdown(const char *line, const char *end, int columns, struct pane_style *style)
{
	while (line < end) {
		const char *esc = memchr(line, '\033', end - line);
		if (!esc) {
			pane_text(line, end - line, columns);
			break;
		}
		columns = pane_text(line, esc - line, columns);
		line = esc + 1;
		if (line == end)
			break;
		if (*line == '[') {
			const char *params = ++line;
			while (line < end && ((unsigned char)*line < '@' || (unsigned char)*line > '~'))
				++line;
			if (line < end && *line == 'm')
				pane_sgr(style, params, line);
		} else if (*line == ']') {
			/* Glow can include OSC 8 hyperlinks. Keep the link text only. */
			while (++line < end && *line != '\a') {
				if (*line == '\033' && line + 1 < end && line[1] == '\\') {
					++line;
					break;
				}
			}
		}
		if (line < end)
			++line;
	}
}

static bool draw_pane_image(int column, int columns, int rows)
{
	int width, height, offset = 0;
	unsigned char *data = file_preview.data + 1;
	if (sscanf((const char *)data, "P6\n%d %d\n255%n", &width, &height, &offset) != 2
	    || offset <= 0 || data[offset] != '\n' || width < 1 || width > columns
	    || height < 1 || height > rows * 2
	    || (size_t)width * height * 3 + (size_t)offset + 2 > file_preview.length
	    || COLORS < COLOR_256 || COLOR_PAIRS <= C_PANE_PREVIEW)
		return FALSE;

	data += offset + 1;
	int prev_fg = -2, prev_bg = -2;
	for (int y = 0; y < height; y += 2) {
		move(y / 2 + 2, column);
		for (int x = 0; x < width; ++x) {
			const unsigned char *top = data + 3 * (y * width + x);
			const unsigned char *bottom = top + width * 3;
			int fg = pane_rgb(top[0], top[1], top[2]);
			int bg = y + 1 < height ? pane_rgb(bottom[0], bottom[1], bottom[2]) : -1;
			if (fg != prev_fg || bg != prev_bg) {
				pane_color(A_NORMAL, fg, bg);
				prev_fg = fg;
				prev_bg = bg;
			}
#ifndef NOLC
			addwstr(L"▀");
#else
			addch('#');
#endif
		}
	}
	attr_set(A_NORMAL, 0, NULL);
	return TRUE;
}

static void load_pane_preview(char *path, const struct stat *info, int width, int height)
{
	xstrsncpy(file_preview.path, path, sizeof(file_preview.path));
	file_preview.info = *info;
	file_preview.width = width;
	file_preview.height = height;
	file_preview.showhidden = cfg.showhidden;
	file_preview.length = 0;
	file_preview.data[0] = '\0';

	if (!previewer) {
		previewer = malloc(PATH_MAX);
		if (previewer)
			mkpath(plgpath, utils[UTIL_NPREVIEW], previewer);
	}
	int pipefd[2];
	if (!previewer || access(previewer, X_OK) || pipe(pipefd) == -1)
		return;

	char cols[16], rows[16];
	snprintf(cols, sizeof(cols), "%d", width);
	snprintf(rows, sizeof(rows), "%d", height);
	char *argv[] = {previewer, path, cols, rows, "rich-pane", cfg.showhidden ? "all" : "", NULL};
	pid_t child;
	int status = spawn_redirected(previewer, argv, pipefd[0], pipefd[1], &child);
	close(pipefd[1]);
	if (!status) {
		while (file_preview.length < PANE_PREVIEW_BYTES) {
			ssize_t n = read(pipefd[0], file_preview.data + file_preview.length,
					 PANE_PREVIEW_BYTES - file_preview.length);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			file_preview.length += n;
		}
	}
	close(pipefd[0]);
	if (!status)
		while (waitpid(child, NULL, 0) == -1 && errno == EINTR) {}
	file_preview.data[file_preview.length] = '\0';
}

static void draw_file_pane(const char *path)
{
	int column = preview_border + 2, width = xcols - column - 1, rows = xlines - 3;
	pane_border(preview_border);
	if (!ndents)
		return;

	const char *name = pdents[cur].name;
	pane_title(column, width, name);
	char target[PATH_MAX];
	struct stat info;
	mkpath(path, name, target);
	if (stat(target, &info) == -1 || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)))
		return;
	if (strcmp(file_preview.path, target) || file_preview.width != width
	    || file_preview.height != rows || file_preview.showhidden != cfg.showhidden
	    || !same_pane_file(&file_preview.info, &info))
		load_pane_preview(target, &info, width, rows);
	if (!file_preview.length)
		return;
	bool markdown = file_preview.length >= 2 && !file_preview.data[0] && file_preview.data[1] == 'M';
	if (!file_preview.data[0] && !markdown) {
		if (!draw_pane_image(column, width, rows))
			mvaddnstr(2, column, "Image preview unavailable", width);
		return;
	}

	const char *data = (const char *)file_preview.data;
	const char *line = data + (markdown ? 2 : 0);
	const char *end = data + file_preview.length;
	struct pane_style style = {A_NORMAL, -1, -1};
	for (int row = 2; row < xlines - 1 && line < end; ++row) {
		const char *next = memchr(line, '\n', end - line);
		const char *stop = next ? next : end;
		if (stop > line && stop[-1] == '\r')
			--stop;
		move(row, column);
		if (markdown)
			pane_markdown(line, stop, width, &style);
		else
			pane_text(line, stop - line, width);
		line = next ? next + 1 : end;
	}
	if (markdown)
		attr_set(A_NORMAL, 0, NULL);
}
