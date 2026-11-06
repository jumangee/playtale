<?php
/**
 * playtale.php — convert a quest-book.ru storygame XML into the PlayTale format.
 *
 * Target: PHP 5.3 (no short array syntax, no JSON_UNESCAPED_UNICODE, explicit
 * UTF-8 charset on every htmlspecialchars / html_entity_decode call).
 *
 * Usage:
 *   php playtale.php game.xml > game.playtale.xml
 *   php playtale.php game.xml out.xml
 *   php playtale.php game.xml out.xml --name="Портал Зла" --index=game19568 \
 *                                    --author="Дмитрий Дударь" --tags="фэнтези, квест"
 *
 * Output: one XML document with three sections.
 *
 *   <header>  everything needed before the first paragraph renders:
 *             start paragraph, parameter and group tables, functions, flags
 *   <data>    one <p> element per paragraph, in ordinal order
 *   <meta>    catalogue fields (from CLI options; a game file carries few)
 *
 * The device walks this document once and writes game.hdr / game.idx / game.dat.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS CONVERTED, AND WHY
 * ---------------------------------------------------------------------------
 *
 * 1. Paragraph reference -> ordinal.
 *    Every 'goto' holds a GUID. Ordinals are dense, so the device reaches a
 *    paragraph with one multiplication instead of a search or a key comparison.
 *
 * 2. Script name -> slot number.
 *    Attributes hold names like local1001_822868323. Measured over 35 games:
 *    11007 references, every one defined inside the SAME <article>, and no name
 *    defined twice. So a paragraph carries its own scripts and a reference is
 *    just a position in that paragraph's <s> list — names disappear entirely.
 *
 * 3. Parameter and group GUID -> ordinal.
 *    Makes player state a flat integer array indexed by parameter ordinal (at
 *    most 322 entries in the corpus): O(1) access, no string keys, ~644 bytes
 *    for a whole game's state. Groups are laid out as contiguous ordinal ranges,
 *    so a group resource count is a loop over [first, first+count) with no
 *    membership test and no per-parameter back-reference.
 *
 * 4. Condition and task JSON -> declarative elements.
 *    Every script in the corpus is one of three shapes. Anything else is kept
 *    verbatim with a warning rather than dropped, so an unrecognised authoring
 *    pattern is visible instead of silently lost.
 *
 * 5. Base64 nested 'if' tasks -> nested elements.
 *    All 530 in the corpus are type:"64" wrapping {action_if, action_ifnot,
 *    action_script}, all three keys always present. The device never needs a
 *    base64 decoder.
 *
 * 6. Entities are decoded and re-escaped.
 *    <text> bodies carry entity-escaped markup (&lt;h6 style=…&gt;) in at least
 *    one published game. Judging text content without decoding first gets it
 *    wrong, and a raw tag scan finds nothing at all.
 *
 * ---------------------------------------------------------------------------
 * THE ONE FORMAT TRAP (verified against expat, do not "optimise" this away)
 * ---------------------------------------------------------------------------
 *
 * In XML, attribute values normalise newlines and tabs to spaces; element
 * content preserves them. The corpus has 16693 newlines and 404 tabs inside
 * paragraph text, and those newlines are meaningful — the platform's BBCode
 * converter turns '\n' into a line break.
 *
 * Therefore: ALL prose is emitted as element content. Attributes carry only
 * numbers, flags and short identifiers. Anything that carries text and could
 * contain a newline must never become an attribute.
 *
 * ---------------------------------------------------------------------------
 * XML PARSING
 * ---------------------------------------------------------------------------
 *
 * XML structure is parsed with DOMDocument. Regular expressions are retained
 * only for the JavaScript and JSON-like source embedded inside <script> nodes;
 * they are not used to identify XML elements or attributes.
 *
 * Articles are still expected to be flat children of <book>, as in the source
 * format. DOM handles CDATA, comments, quoted attributes and text boundaries,
 * so strings that merely look like XML tags inside script or text content cannot
 * accidentally split the document.
 *
 * The original handler fingerprint is retained over the exact source bytes
 * between <handlers> and </handlers>, because changing that hash algorithm
 * would change an observable platform-compatibility value.
 */

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
if (!defined('IN_PHPBB')) {
	header('Location: /online/');
	exit;
}

function playtaleConvert($raw, $opts) {
	if (!$opts) {
		$opts = array();
		/*foreach (array('name', 'author', 'index', 'game_id', 'tags', 'cover', 'likes',
					   'dislikes', 'stars', 'desc', 'level', 'created') as $k) {
			$opts[$k] = '';
		}*/
	}
    //$opts['help'] = false;
    /*$positional = array();

    for ($i = 1; $i < count($argv); $i++) {
        $a = $argv[$i];
        if ($a === '--help' || $a === '-h') {
            $opts['help'] = true;
        } else if (substr($a, 0, 2) === '--') {
            $eq = strpos($a, '=');
            if ($eq === false) {
                $opts[substr($a, 2)] = '';
            } else {
                $opts[substr($a, 2, $eq - 2)] = substr($a, $eq + 1);
            }
        } else {
            $positional[] = $a;
        }
    }

    if ($opts['help'] || count($positional) < 1) {
        fwrite(STDERR, "playtale.php - convert a storygame XML into the PlayTale format\n\n");
        fwrite(STDERR, "Usage: php playtale.php <input.xml> [output.xml] [options]\n\n");
        fwrite(STDERR, "Options (all optional; they only fill the <meta> section):\n");
        fwrite(STDERR, "  --name= --author= --index= --game_id= --tags= --cover=\n");
        fwrite(STDERR, "  --likes= --dislikes= --stars= --desc= --level= --created=\n");
        return $opts['help'] ? 0 : 2;
    }

    $inPath  = $positional[0];
    $outPath = isset($positional[1]) ? $positional[1] : '';

    if (!is_file($inPath) || !is_readable($inPath)) {
        fwrite(STDERR, "error: cannot read input: $inPath\n");
        return 2;
    }

    $raw = file_get_contents($inPath);
    if ($raw === false || $raw === '') {
        fwrite(STDERR, "error: input is empty or unreadable: $inPath\n");
        return 2;
    }*/

    $converter = new PlayTaleConverter($opts);
    try {
        $xml = $converter->convert($raw);
    } catch (Exception $e) {
        fwrite(STDERR, "error: " . $e->getMessage() . "\n");
        return 1;
    }

    return $xml;

    /*foreach ($converter->warnings as $w) {
        //fwrite(STDERR, "warning: $w\n");
    }
    fwrite(STDERR, "converted " . $converter->paragraphCount . " paragraphs, "
                 . $converter->itemCount . " parameters in " . $converter->groupCount
                 . " groups\n");

    return 0;*/
}

// ---------------------------------------------------------------------------
// Converter
// ---------------------------------------------------------------------------

class PlayTaleConverter {

    const FORMAT_VERSION = 1;

    /** Catalogue fields supplied on the command line. */
    private $opts;

    /** Collected non-fatal problems; emitted as XML comments and to stderr. */
    public $warnings = array();

    /** uid => ordinal */
    private $paraOrdinal = array();
    /** entity-or-raw uid => ordinal, for keywords and resources groups */
    private $groupOrdinal = array();
    /** uid => ordinal */
    private $itemOrdinal = array();
    /** "k"|"r" . uid-without-dashes => item ordinal, for 's' expressions */
    private $itemByFlat = array();
    /** uid => ordinal */
    private $funcOrdinal = array();

    /** Parallel arrays indexed by ordinal. */
    private $groups = array();
    private $items = array();
    public $groupCount = 0;
    public $itemCount = 0;
    public $paragraphCount = 0;

    /** Game-level facts gathered from the mitril bootstrap. */
    private $rawSetup = null;
    private $startUid = '';
    private $bookGuid = '';
    private $handlerNames = array();
    private $handlerFingerprint = '';
    private $hasBootstrap = false;

    public function __construct($opts) {
        $defaults = array(
            'game_id'  => '',
            'index'    => '',
            'name'     => '',
            'author'   => '',
            'tags'     => '',
            'cover'    => '',
            'likes'    => '',
            'dislikes' => '',
            'stars'    => '',
            'desc'     => '',
            'level'    => '',
            'created'  => ''
        );

        $this->opts = array_merge($defaults, (array)$opts);
    }

    private function warn($msg) {
        $this->warnings[] = $msg;
    }

    // -----------------------------------------------------------------------
    // Top level
    // -----------------------------------------------------------------------

    /**
     * @param string $raw the whole input file.
     * @return string the PlayTale document.
     */
    public function convert($raw) {
        if (substr($raw, 0, 3) === "\xEF\xBB\xBF") {
            $raw = substr($raw, 3);           // strip a BOM if present
        }

        $dom = $this->loadXml($raw);
        $articles = $this->splitArticles($dom);

        // The mitril article becomes the header; it is not a paragraph.
        $bootstrapArticle = null;
        $order = array();
        foreach ($articles as $a) {
            if ($a['id'] === 'mitril') {
                $bootstrapArticle = $a['node'];
                continue;
            }
            $order[] = $a['id'];
        }

        if (count($order) === 0) {
            throw new Exception('no story paragraphs found - not a storygame file?');
        }

        foreach ($order as $i => $uid) {
            $this->paraOrdinal[$uid] = $i;
        }
        $this->paragraphCount = count($order);

        if ($bootstrapArticle !== null) {
            $this->parseBootstrap($bootstrapArticle);
        } else {
            $this->warn('no <article id="mitril">: the game has no parameter setup');
        }

        $this->buildParameterTables();
        $this->parseHandlers($dom, $raw);

        return $this->emit($articles);
    }

    /**
     * Load and validate the source XML.
     *
     * External entity loading is disabled for PHP 5.3 compatibility and to
     * prevent network/local-file entity expansion. LIBXML_NONET is an
     * additional defence; entity substitution is intentionally not enabled.
     */
    private function loadXml($raw) {
        if (!class_exists('DOMDocument')) {
            throw new Exception('DOM extension is required to parse the source XML');
        }

        if ($raw === '') {
            throw new Exception('empty XML document');
        }

        $oldErrors = libxml_use_internal_errors(true);
        libxml_clear_errors();

        $oldEntityLoader = null;
        if (function_exists('libxml_disable_entity_loader')) {
            $oldEntityLoader = @libxml_disable_entity_loader(true);
        }

        $dom = new DOMDocument('1.0', 'UTF-8');
        $dom->preserveWhiteSpace = true;
        $dom->resolveExternals = false;
        $dom->substituteEntities = false;
        $dom->validateOnParse = false;

        $flags = defined('LIBXML_NONET') ? LIBXML_NONET : 0;
        $ok = $dom->loadXML($raw, $flags);
        $errors = libxml_get_errors();

        libxml_clear_errors();
        if ($oldEntityLoader !== null) {
            @libxml_disable_entity_loader($oldEntityLoader);
        }
        libxml_use_internal_errors($oldErrors);

        if (!$ok) {
            $message = 'invalid XML document';
            if (count($errors) > 0) {
                $message .= ': ' . trim($errors[0]->message)
                          . ' (line ' . $errors[0]->line . ')';
            }
            throw new Exception($message);
        }

        return $dom;
    }

    /**
     * Collect flat <article> children of <book>, preserving source order.
     * The original format expects articles to be direct children of <book>.
     */
    private function splitArticles($dom) {
        $out = array();
        $books = $dom->getElementsByTagName('book');

        if ($books->length === 0) {
            throw new Exception('book element not found');
        }

        $book = $books->item(0);
        $this->bookGuid = $book->getAttribute('guid');

        foreach ($book->childNodes as $node) {
            if ($node->nodeType !== XML_ELEMENT_NODE || $node->nodeName !== 'article') {
                continue;
            }

            $id = $node->getAttribute('id');
            if ($id === '') {
                $this->warn('an <article> has no id and was skipped');
                continue;
            }

            $out[] = array('id' => $id, 'node' => $node);
        }

        // Keep the old diagnostic for nested/future article layouts.
        $declared = $book->getElementsByTagName('article')->length;
        if (count($out) !== $declared) {
            $this->warn('found ' . count($out) . ' articles but ' . $declared
                      . ' <article> start tags; the file may be malformed or a'
                      . ' future export may nest articles');
        }

        return $out;
    }

    /**
     * Pull rawSetup, the start paragraph, the handler list and the platform
     * scalars out of the mitril article.
     *
     * Its onload script is: var mitrilSetup = (function(rawSetup){...})({JSON});
     * followed by function mitrilController(){...}. The start paragraph exists
     * ONLY as a showArticle("<guid>") call inside that JavaScript - it is not in
     * the markup anywhere - so it has to be scraped here or the game cannot
     * begin.
     */
    private function parseBootstrap($mitrilArticle) {
        $this->hasBootstrap = true;

        $scripts = $mitrilArticle->getElementsByTagName('script');
        if ($scripts->length === 0) {
            $this->warn('the mitril article has no <script>; no parameter setup extracted');
            return;
        }

        // DOM textContent returns CDATA contents without the CDATA delimiters.
        $body = $scripts->item(0)->textContent;

        // rawSetup: anchored on the controller function that always follows it.
        if (preg_match('/\}\)\((\{.*?\})\);\s*function\s+mitrilController/s', $body, $rs)) {
            $decoded = json_decode($rs[1], true);
            if (is_array($decoded)) {
                $this->rawSetup = $decoded;
            } else {
                $this->warn('rawSetup JSON did not decode; the parameter setup is unavailable');
            }
        } else {
            $this->warn('rawSetup JSON not found in the mitril bootstrap');
        }

        // Start paragraph: the first showArticle call.
        if (preg_match_all('/showArticle\(\s*[\'"]([0-9a-fA-F-]{36})[\'"]/', $body, $sa)) {
            $this->startUid = $sa[1][0];
            $distinct = array_unique($sa[1]);
            if (count($distinct) > 1) {
                $this->warn('the bootstrap names ' . count($distinct)
                          . ' distinct showArticle targets; the first was used');
            }
        } else {
            $this->warn('no showArticle target found; the start paragraph is unknown');
        }

        // Platform scalars, used only to fill <meta> when not given on the CLI.
        if ($this->opts['game_id'] === '' && preg_match('/game_id\s*:\s*(\d+)/', $body, $g)) {
            $this->opts['game_id'] = $g[1];
        }
        if ($this->opts['index'] === ''
            && preg_match('/index\s*:\s*[\'"]([^\'"]+)[\'"]/', $body, $ix)) {
            $this->opts['index'] = $ix[1];
        }

        // The <handlers> block is a sibling of <article>, not part of it, so it
        // is collected separately by parseHandlers().
    }

    /**
     * Build the parameter and group ordinal tables.
     *
     * Layout rule: groups keep source order and each group's items occupy one
     * contiguous ordinal range. That is what makes a group resource count a
     * plain range loop on the device.
     */
    private function buildParameterTables() {
        if ($this->rawSetup === null) return;

        foreach (array('keywords' => 'k', 'resources' => 'r') as $key => $type) {
            if (!isset($this->rawSetup[$key]) || !is_array($this->rawSetup[$key])) continue;

            foreach ($this->rawSetup[$key] as $g) {
                if (!is_array($g) || !isset($g['uid']) || $g['uid'] === '') continue;

                $gOrd = $this->groupCount++;
                $this->groupOrdinal[(string)$g['uid']] = $gOrd;

                $first = $this->itemCount;
                $count = 0;
                $list  = isset($g['items']) && is_array($g['items']) ? $g['items'] : array();

                foreach ($list as $it) {
                    if (!is_array($it) || !isset($it['uid']) || $it['uid'] === '') continue;

                    $ord = $this->itemCount++;
                    $this->itemOrdinal[(string)$it['uid']] = $ord;
                    $this->itemByFlat[$type . str_replace('-', '', $it['uid'])] = $ord;

                    $this->items[$ord] = array(
                        'ordinal' => $ord,
                        'group'   => $gOrd,
                        'type'    => $type,
                        'name'    => isset($it['name']) ? $it['name'] : '',
                        'start'   => $this->itemStart($type, $it),
                        'limit'   => isset($it['limit']) ? $this->parseLimit($it['limit']) : 0,
                    );
                    $count++;
                }

                $this->groups[$gOrd] = array(
                    'ordinal' => $gOrd,
                    'name'    => isset($g['name']) ? $g['name'] : '',
                    'type'    => $type,
                    'visible' => $this->mixBool(isset($g['visible']) ? $g['visible'] : 'false') ? 1 : 0,
                    'limit'   => isset($g['limit']) ? $this->parseLimit($g['limit']) : -1,
                    'first'   => $first,
                    'count'   => $count,
                );
            }
        }

        // Functions, invoked by a 'fun' task.
        if (isset($this->rawSetup['functions']) && is_array($this->rawSetup['functions'])) {
            $n = 0;
            foreach ($this->rawSetup['functions'] as $f) {
                if (!is_array($f) || !isset($f['func_uid'])) continue;
                $this->funcOrdinal[(string)$f['func_uid']] = $n++;
            }
        }
    }

    /** Keyword items start owned or not; resource items start at a number. */
    private function itemStart($type, $it) {
        $raw = isset($it['start']) ? $it['start'] : '0';
        if ($type === 'k') return $this->mixBool($raw) ? 1 : 0;
        return is_numeric($raw) ? (int)$raw : 0;
    }

    /** -1 means "no limit"; the source distinguishes false from 0. */
    private function parseLimit($v) {
        if ($v === false || $v === 'false' || $v === '') return -1;
        if (is_numeric($v)) return (int)$v;
        return -1;
    }

    /** The platform's parseMixBoolean: 'true'/'false', otherwise parseInt. */
    private function mixBool($v) {
        if ($v === true  || $v === 'true')  return true;
        if ($v === false || $v === 'false') return false;
        return ((int)$v) !== 0;
    }

    // -----------------------------------------------------------------------
    // Emit
    // -----------------------------------------------------------------------

    private function emit($articles) {
        // Build every section before writing warnings: emitHeader(),
        // emitParagraph() and escaping helpers can add warnings themselves.
        $header = $this->emitHeader();
        $paragraphs = array();

        foreach ($articles as $a) {
            if ($a['id'] === 'mitril') continue;
            $paragraphs[] = $this->emitParagraph($a['node'], $a['id']);
        }

        $meta = $this->emitMeta();

        $o = array();
        $o[] = '<?xml version="1.0" encoding="UTF-8"?>' . "\n";
        $o[] = '<storygame v="' . self::FORMAT_VERSION . '">' . "\n";

        foreach ($this->warnings as $w) {
            $o[] = '  <!-- warning: ' . $this->escComment($w) . ' -->' . "\n";
        }

        $o[] = $header;
        $o[] = "  <data>\n";
        foreach ($paragraphs as $paragraph) {
            $o[] = $paragraph;
        }
        $o[] = "  </data>\n";
        $o[] = $meta;
        $o[] = "</storygame>\n";

        return implode('', $o);
    }

    private function emitHeader() {
        $s  = "  <header>\n";

        $s .= '    <book guid="' . $this->escAttr($this->bookGuid) . '"';
        if ($this->opts['game_id'] !== '') $s .= ' game_id="' . $this->escAttr($this->opts['game_id']) . '"';
        if ($this->opts['index']   !== '') $s .= ' index="'   . $this->escAttr($this->opts['index'])   . '"';
        $s .= "/>\n";

        $startOrd = isset($this->paraOrdinal[$this->startUid])
                  ? $this->paraOrdinal[$this->startUid] : -1;
        if ($startOrd < 0 && $this->startUid !== '') {
            $this->warn('the start paragraph "' . $this->startUid
                      . '" is not an article in this file');
        }
        $s .= '    <start p="' . $startOrd . "\"/>\n";

        if (isset($this->paraOrdinal['_mitril_triggers'])) {
            $s .= '    <triggers p="' . $this->paraOrdinal['_mitril_triggers'] . "\"/>\n";
        }

        $s .= '    <counts g="' . $this->groupCount
            . '" i="' . $this->itemCount
            . '" p="' . $this->paragraphCount
            . '" f="' . count($this->funcOrdinal) . "\"/>\n";

        /*$s .= '    <platform bootstrap="' . ($this->hasBootstrap ? 1 : 0) . '"'
            . ' handlers="' . $this->escAttr($this->handlerFingerprint) . '"'
            . ">\n";
        foreach ($this->handlerNames as $n) {
            $s .= '      <hd n="' . $this->escAttr($n) . "\"/>\n";
        }
        $s .= "    </platform>\n";*/

        // Groups.
        $s .= "    <groups>\n";
        foreach ($this->groups as $g) {
            $s .= '      <g n="' . $g['ordinal'] . '" t="' . $g['type'] . '"'
                . ' vis="' . $g['visible'] . '"'
                . ' lim="' . ($g['limit'] < 0 ? 'none' : $g['limit']) . '"'
                . ' first="' . $g['first'] . '" count="' . $g['count'] . '"'
                . ' name="' . $this->escAttr($g['name']) . "\"/>\n";
        }
        $s .= "    </groups>\n";

        // Parameters: 'start' is the value a fresh game begins with.
        $s .= "    <params>\n";
        foreach ($this->items as $it) {
            $s .= '      <pm n="' . $it['ordinal'] . '" g="' . $it['group'] . '" t="' . $it['type'] . '"'
                . ' start="' . $it['start'] . '" lim="' . $it['limit'] . '"'
                . ' name="' . $this->escAttr($it['name']) . "\"/>\n";
        }
        $s .= "    </params>\n";

        // Functions.
        $s .= "    <funcs>\n";
        if ($this->rawSetup !== null && isset($this->rawSetup['functions'])
            && is_array($this->rawSetup['functions'])) {
            foreach ($this->rawSetup['functions'] as $f) {
                if (!is_array($f) || !isset($f['func_uid'])) continue;
                $ord    = $this->funcOrdinal[(string)$f['func_uid']];
                $name   = isset($f['func_name']) ? $f['func_name'] : '';
                $script = isset($f['func_script']) ? $f['func_script'] : '[]';
                $tasks  = json_decode($script, true);

                $s .= '      <fn n="' . $ord . '" name="' . $this->escAttr($name) . "\">\n";
                if (is_array($tasks)) {
                    $s .= $this->emitTasks($tasks, '        ');
                } else {
                    $this->warn('function "' . $name . '" has an unparseable script');
                }
                $s .= "      </fn>\n";
            }
        }
        $s .= "    </funcs>\n";

        $s .= "  </header>\n";
        return $s;
    }

    private function emitMeta() {
        $s = "  <meta>\n";
        $map = array(
            'game_id'  => 'game_id',
            'index'    => 'index_name',
            'name'     => 'fullname',
            'author'   => 'author_name',
            'tags'     => 'tags',
            'cover'    => 'cover',
            'likes'    => 'likes',
            'dislikes' => 'dislikes',
            'stars'    => 'stars',
            'level'    => 'level',
            'created'  => 'created',
            'desc'     => 'desc',
        );
        foreach ($map as $opt => $element) {
            if (isset($this->opts[$opt]) && $this->opts[$opt] !== '') {
                $s .= '    <' . $element . '>' . $this->escText($this->opts[$opt])
                    . '</' . $element . ">\n";
            }
        }
        $s .= '    <guid>' . $this->escText($this->bookGuid) . "</guid>\n";
        $s .= "  </meta>\n";
        return $s;
    }

    // -----------------------------------------------------------------------
    // Paragraph conversion
    // -----------------------------------------------------------------------

    /**
     * Convert one <article> into one <p> payload.
     *
     * Script elements are collected first to preserve slot numbering. DOM keeps
     * CDATA and text contents separate from real XML elements, so markup-like
     * strings inside JavaScript are never mistaken for <text> or <action> nodes.
     */
    private function emitParagraph($article, $uid) {
        $ord = $this->paraOrdinal[$uid];

        // --- scripts, in document order; the position becomes the slot number ---
        $slotByType = array();
        $slotXml    = array();
        $slot       = 0;

        $scriptNodes = $article->getElementsByTagName('script');
        foreach ($scriptNodes as $scriptNode) {
            $type = $scriptNode->getAttribute('type');
            if ($type === 'onload') continue;      // only the mitril bootstrap

            $body = $scriptNode->textContent;
            $role = 'l';
            if ($type === 'preload')     $role = 'p';
            else if ($type === 'unload') $role = 'u';

            if ($type !== '') $slotByType[$type] = $slot;
            $slotXml[] = $this->emitScript($body, $role, $slot);
            $slot++;
        }

        // --- text blocks ---
        $texts = array();
        $textNodes = $article->getElementsByTagName('text');
        foreach ($textNodes as $textNode) {
            $attrs = $this->domAttributes($textNode);
            $texts[] = array(
                'if'      => $this->slotRef($attrs, 'if',    $slotByType, $uid),
                'ifnot'   => $this->slotRef($attrs, 'ifnot', $slotByType, $uid),
                'spoiler' => $textNode->getAttribute('spoiler'),
                'class'   => $textNode->getAttribute('class'),
                'id'      => $textNode->getAttribute('id'),
                'text'    => $this->innerXmlDecoded($textNode),
            );
        }

        // --- choices ---
        $actions = array();
        $actionNodes = $article->getElementsByTagName('action');
        foreach ($actionNodes as $actionNode) {
            $attrs = $this->domAttributes($actionNode);

            $target = '';
            if (isset($attrs['goto']) && $attrs['goto'] !== '')      $target = $attrs['goto'];
            else if (isset($attrs['go']) && $attrs['go'] !== '')     $target = $attrs['go'];

            $gotoOrd = -1;
            if ($target !== '') {
                if (isset($this->paraOrdinal[$target])) {
                    $gotoOrd = $this->paraOrdinal[$target];
                } else {
                    $this->warn("paragraph $uid: goto target $target does not exist");
                }
            }

            $actions[] = array(
                'goto'  => $gotoOrd,
                'do'    => $this->slotRef($attrs, 'do',    $slotByType, $uid),
                'if'    => $this->slotRef($attrs, 'if',    $slotByType, $uid),
                'ifnot' => $this->slotRef($attrs, 'ifnot', $slotByType, $uid),
                'class' => isset($attrs['class']) ? $attrs['class'] : '',
                'label' => $this->innerXmlDecoded($actionNode),
            );
        }

        // Images and music are intentionally skipped by the current converter.
        // Keep these arrays so the output/flag behavior remains unchanged.
        $imgs = array();
        $music = array();

        // --- flags ---
        $flags = 0;
        if (count($actions) === 0)        $flags |= 1;    // an ending: no visible choices
        if (count($imgs) > 0)             $flags |= 2;    // has images
        if ($slot > 0)                    $flags |= 4;    // has scripts
        if ($uid === $this->startUid)     $flags |= 8;    // the start paragraph
        if ($uid === '_mitril_triggers')  $flags |= 16;   // appended to every paragraph

        // --- emit ---
        $s = '    <p n="' . $ord . '" f="' . $flags . "\">\n";
        foreach ($slotXml as $x) $s .= $x;

        foreach ($texts as $t) {
            $s .= '      <t';
            if ($t['if'] >= 0)        $s .= ' i="'  . $t['if'] . '"';
            if ($t['ifnot'] >= 0)     $s .= ' n="'  . $t['ifnot'] . '"';
            if ($t['spoiler'] !== '') $s .= ' sp="' . $this->escAttr($t['spoiler']) . '"';
            if ($t['class'] !== '')   $s .= ' cl="' . $this->escAttr($t['class']) . '"';
            if ($t['id'] !== '')      $s .= ' id="' . $this->escAttr($t['id']) . '"';
            $s .= '>' . $this->escText($t['text']) . "</t>\n";     // prose as content
        }

        foreach ($actions as $a) {
            $s .= '      <a';
            if ($a['goto'] >= 0)      $s .= ' g="' . $a['goto'] . '"';
            if ($a['do'] >= 0)        $s .= ' d="' . $a['do'] . '"';
            if ($a['if'] >= 0)        $s .= ' i="' . $a['if'] . '"';
            if ($a['ifnot'] >= 0)     $s .= ' n="' . $a['ifnot'] . '"';
            if ($a['class'] !== '')   $s .= ' c="' . $this->escAttr($a['class']) . '"';
            $s .= '>' . $this->escText($a['label']) . "</a>\n";    // prose as content
        }

        foreach ($imgs as $i) {
            if ($i['name'] !== '') $s .= '      <i n="' . $this->escAttr($i['name']) . '">';
            else $s .= '      <i>';
            $s .= $this->escText($i['url']) . "</i>\n";
        }

        foreach ($music as $m) {
            $s .= '      <m>' . $this->escText($m) . "</m>\n";
        }

        $s .= "    </p>\n";
        return $s;
    }

    /**
     * Resolve an if / ifnot / do attribute to a slot number.
     *
     * The attribute holds a script name defined in the same article. Measured
     * across 35 games: 11007 references, none resolving to another paragraph,
     * and no name defined twice — so a position is a complete address. A name
     * that matches nothing is reported rather than silently dropped.
     */
    private function slotRef($attrs, $key, $slotByType, $uid) {
        if (!isset($attrs[$key]) || $attrs[$key] === '') return -1;
        $name = $attrs[$key];
        if (isset($slotByType[$name])) return $slotByType[$name];
        $this->warn("paragraph $uid: $key=\"$name\" matches no script in this paragraph");
        return -1;
    }

    // -----------------------------------------------------------------------
    // Script conversion
    // -----------------------------------------------------------------------

    /**
     * Turn one script body into a declarative element.
     *
     * Three shapes cover the whole corpus. Anything else is preserved verbatim
     * with a warning, so an unrecognised authoring pattern stays visible.
     */
    private function emitScript($body, $role, $slot) {
        $t = trim($body);

        // A condition predicate, optionally negated.
        if (preg_match('/^(?:return\s+)?(!?)mitril\.checkCondition\(\s*vars\.items\s*,'
                     . '\s*(.+?)\s*,\s*(true|false)\s*\)\s*;?$/s', $t, $m)) {
            return $this->emitCondition($m[2], $m[3] === 'true', $m[1] === '!', $role, $slot);
        }

        // A task list.
        if (preg_match('/^mitril\.scriptWrap\(\s*vars\s*,\s*(\[.*\])\s*\)\s*;?$/s', $t, $m)) {
            $tasks = json_decode($m[1], true);
            if ($tasks === null) {
                $this->warn("slot $slot: task list JSON did not decode; kept verbatim");
                return $this->rawScript($body, $role, $slot);
            }
            $s  = '      <s k="t"' . $this->roleAttr($role) . ' slot="' . $slot . "\">\n";
            $s .= $this->emitTasks($tasks, '        ');
            $s .= "      </s>\n";
            return $s;
        }

        $this->warn("slot $slot: script is not a recognised declarative shape; kept verbatim");
        return $this->rawScript($body, $role, $slot);
    }

    private function roleAttr($role) {
        return $role === 'l' ? '' : ' r="' . $role . '"';
    }

    private function rawScript($body, $role, $slot) {
        return '      <s k="x"' . $this->roleAttr($role) . ' slot="' . $slot . '">'
             . $this->escText($body) . "</s>\n";
    }

    /**
     * Emit a condition.
     *
     * The reference semantics are preserved rather than normalised, because
     * normalising them is easy to get subtly wrong:
     *
     *   crit=true   AND: every expression true      OR: any expression true
     *   crit=false  AND: every expression false     OR: any expression false
     *
     * and the whole result may be negated by a leading '!' on the return. The
     * runtime implements this algorithm; the attributes carry its parameters.
     */
    private function emitCondition($condJson, $crit, $negated, $role, $slot) {
        $cond = json_decode($condJson, true);
        if (!is_array($cond)) {
            $this->warn("slot $slot: condition JSON did not decode; kept verbatim");
            return $this->rawScript(
                'return mitril.checkCondition(vars.items, ' . $condJson . ', '
                . ($crit ? 'true' : 'false') . ');', $role, $slot);
        }

        $or   = 0;
        $list = $cond;
        if (isset($cond['or'])) {                       // OR form: {"or":1,"c":[…]}
            $or   = 1;
            $list = isset($cond['c']) && is_array($cond['c']) ? $cond['c'] : array();
        }

        $s  = '      <s k="c"' . $this->roleAttr($role) . ' slot="' . $slot . '"'
            . ' or="' . $or . '" crit="' . ($crit ? 1 : 0) . '" not="' . ($negated ? 1 : 0) . "\">\n";
        foreach ($list as $e) {
            if (is_array($e)) $s .= $this->emitExpr($e, '        ');
        }
        $s .= "      </s>\n";
        return $s;
    }

    /**
     * One condition expression.
     *
     *   type k: a flag is set
     *   type r: a counter is >= amnt, or == amnt when 'eq' is present
     *   type s: a text expression, whose k/r identifiers are rewritten to ordinals
     *
     * In 'r', 'amnt' may itself name another parameter (the reference tells the
     * two apart by testing isNaN), so a non-numeric amount becomes an ordinal
     * reference in its own attribute.
     */
    private function emitExpr($e, $indent) {
        $type = isset($e['type']) ? $e['type'] : '';
        $val  = isset($e['val'])  ? $e['val']  : '';

        if ($type === 's') {
            // Expression source: kept as element content, identifiers rewritten.
            return $indent . '<e t="s">' . $this->escText($this->rewriteExpression($val)) . "</e>\n";
        }

        $s = $indent . '<e t="' . $this->escAttr($type) . '"';

        if ($type === 'k') {
            $s .= ' v="' . $this->itemRef($val) . '"';
        } else if ($type === 'r') {
            $s .= ' v="' . $this->itemRef($val) . '"';
            $amnt = isset($e['amnt']) ? $e['amnt'] : null;
            if ($amnt !== null && $amnt !== '') {
                if (is_numeric($amnt)) {
                    $s .= ' a="' . (int)$amnt . '"';
                } else {
                    $s .= ' ap="' . $this->itemRef($amnt) . '"';
                }
            }
            if (!empty($e['eq'])) $s .= ' eq="1"';
        } else {
            // An unknown expression type: keep the value so nothing is lost.
            $s .= ' v="' . $this->escAttr(is_scalar($val) ? $val : json_encode($val)) . '"';
            $this->warn('unknown condition expression type "' . $type . '"');
        }

        $s .= "/>\n";
        return $s;
    }

    /**
     * Rewrite 'k<32hex>' / 'r<32hex>' identifiers in an expression to ordinals.
     *
     * These are parameter uids with the dashes removed, prefixed by the group
     * type ('k' or 'r'). Verified: every identifier in the corpus resolves this
     * way. An unresolvable one becomes 'k?' / 'r?' so the output stays parseable
     * and the problem is obvious.
     */
    private function rewriteExpression($expr) {
        return preg_replace_callback('/\b([kr])([0-9a-f]{32})\b/',
                                     array($this, 'rewriteExpressionMatch'), $expr);
    }

    private function rewriteExpressionMatch($m) {
        $key = $m[1] . $m[2];
        if (isset($this->itemByFlat[$key])) {
            return $m[1] . $this->itemByFlat[$key];
        }
        $this->warn('an expression references unknown parameter ' . $key);
        return $m[1] . '?';
    }

    // -----------------------------------------------------------------------
    // Task conversion
    // -----------------------------------------------------------------------

    /**
     * Emit a task list.
     *
     * Field roles by class, all verified against the corpus:
     *
     *   group-valued  del nlg rnd rng                        val is a GROUP uid
     *   item-valued   inc dec set add sub mlp div eql rnr rrr  val is a PARAM uid
     *   fun           fun                                    val is a FUNCTION uid
     *   navigation    goto                                   val is a PARAGRAPH uid
     *   checkpoint    save                                   val stays text (inert offline)
     *   achievement   achi                                   val is a small number
     *   conditional   if                                     base64-wrapped nested JSON
     *
     * 'param', when present, always names a PARAM whose value is read. 'amnt' is
     * a literal amount. Getting these two the wrong way round would silently
     * change a game's arithmetic, so each class is listed explicitly.
     */
    private function emitTasks($tasks, $indent) {
        $s = '';
        if (!is_array($tasks)) return $s;

        foreach ($tasks as $t) {
            if (!is_array($t)) continue;
            $class = isset($t['class']) ? $t['class'] : '';
            if ($class === '') continue;

            $s .= $indent . '<k c="' . $this->escAttr($class) . '"';

            if (isset($t['type']) && $t['type'] !== '' && $class !== 'if') {
                $s .= ' t="' . $this->escAttr($t['type']) . '"';
            }

            switch ($class) {
                case 'del':
                case 'nlg':
                case 'rnd':
                case 'rng':
                case 'rrg':
                    // Group-valued: the reference looks these up in
                    // mitrilSetup.groups, not in items. 'rng' and 'rrg' also
                    // carry a 'param' naming the item whose value is read.
                    $s .= ' v="' . $this->groupRef(isset($t['val']) ? $t['val'] : '') . '"';
                    break;

                case 'goto':
                    $s .= ' v="' . $this->paraRef(isset($t['val']) ? $t['val'] : '') . '"';
                    break;

                case 'fun':
                    $v = isset($t['val']) ? (string)$t['val'] : '';
                    if (isset($this->funcOrdinal[$v])) {
                        $s .= ' v="' . $this->funcOrdinal[$v] . '"';
                    } else {
                        $s .= ' v="-1"';
                        $this->warn("a fun task references an unknown function $v");
                    }
                    break;

                case 'achi':
                    // Achievement ids are small numbers and stay as they are.
                    $s .= ' v="' . (int)(isset($t['val']) ? $t['val'] : 0) . '"';
                    break;

                case 'save':
                    // Checkpoint saving is server-side and out of scope offline,
                    // but the value is kept so the information is not lost.
                    $s .= ' v="' . $this->escAttr(isset($t['val']) ? $t['val'] : '') . '"';
                    break;

                case 'if':
                    $s .= ">\n";
                    $s .= $this->emitIfTask($t, $indent . '  ');
                    $s .= $indent . "</k>\n";
                    continue 2;                       // continue the foreach

                case 'back':
                    // Takes no operand: the runtime uses vars.lastarticle.
                    break;

                default:
                    // inc dec set add sub mlp div eql rnr rrr
                    if (isset($t['val'])) {
                        $s .= ' v="' . $this->itemRef($t['val']) . '"';
                    }
                    break;
            }

            if (isset($t['amnt']) && $t['amnt'] !== '' && !is_array($t['amnt'])) {
                $s .= ' a="' . (is_numeric($t['amnt']) ? (int)$t['amnt']
                                                       : $this->escAttr($t['amnt'])) . '"';
            }

            if (isset($t['param']) && $t['param'] !== '') {
                $s .= ' p="' . $this->itemRef($t['param']) . '"';
            }

            $s .= "/>\n";
        }

        return $s;
    }

    /**
     * A conditional task.
     *
     * The reference decodes base64 when 'type' is "64", then parses an object
     * with action_if, action_ifnot and action_script — all three always present
     * in the 530 found in the corpus. A branch whose source is "[]" is skipped
     * by the reference, which is what <none/> records.
     */
    private function emitIfTask($task, $indent) {
        $val  = isset($task['val'])  ? $task['val']  : '';
        $type = isset($task['type']) ? $task['type'] : '';

        if ($type === '64') {
            $decoded = base64_decode($val, true);
            if ($decoded === false) {
                $this->warn('an "if" task has undecodable base64; kept verbatim');
                return $indent . '<raw>' . $this->escText($val) . "</raw>\n";
            }
        } else {
            $decoded = $val;
        }

        $obj = json_decode($decoded, true);
        if (!is_array($obj)) {
            $this->warn('an "if" task payload did not decode; kept verbatim');
            return $indent . '<raw>' . $this->escText($decoded) . "</raw>\n";
        }

        $s  = $indent . "<if>\n";
        $s .= $this->emitIfBranch(isset($obj['action_if'])    ? $obj['action_if']    : '',
                                  true,  $indent . '  ');
        $s .= $indent . "</if>\n";

        $s .= $indent . "<ifnot>\n";
        $s .= $this->emitIfBranch(isset($obj['action_ifnot']) ? $obj['action_ifnot'] : '',
                                  false, $indent . '  ');
        $s .= $indent . "</ifnot>\n";

        $s .= $indent . "<then>\n";
        $inner = isset($obj['action_script']) ? $obj['action_script'] : '';
        $tasks = ($inner === '' || trim($inner) === '[]') ? array() : json_decode($inner, true);
        if (is_array($tasks)) {
            $s .= $this->emitTasks($tasks, $indent . '  ');
        } else {
            $this->warn('an "if" task action_script did not decode');
        }
        $s .= $indent . "</then>\n";

        return $s;
    }

    /** One branch of a conditional task. */
    private function emitIfBranch($json, $crit, $indent) {
        // The reference treats a source of 2 characters or fewer ("[]") as absent.
        if (!is_string($json) || strlen(trim($json)) <= 2) {
            return $indent . "<none/>\n";
        }

        $cond = json_decode($json, true);
        if (!is_array($cond)) {
            $this->warn('an "if" branch condition did not decode');
            return $indent . "<none/>\n";
        }

        $or   = 0;
        $list = $cond;
        if (isset($cond['or'])) {
            $or   = 1;
            $list = isset($cond['c']) && is_array($cond['c']) ? $cond['c'] : array();
        }

        $s = $indent . '<c or="' . $or . '" crit="' . ($crit ? 1 : 0) . "\">\n";
        foreach ($list as $e) {
            if (is_array($e)) $s .= $this->emitExpr($e, $indent . '  ');
        }
        $s .= $indent . "</c>\n";
        return $s;
    }

    // -----------------------------------------------------------------------
    // Reference helpers
    // -----------------------------------------------------------------------

    private function itemRef($uid) {
        if ($uid === '' || $uid === null) return -1;
        $uid = (string)$uid;
        if (isset($this->itemOrdinal[$uid])) return $this->itemOrdinal[$uid];
        $this->warn("unknown parameter uid $uid");
        return -1;
    }

    private function groupRef($uid) {
        if ($uid === '' || $uid === null) return -1;
        $uid = (string)$uid;
        if (isset($this->groupOrdinal[$uid])) return $this->groupOrdinal[$uid];
        $this->warn("unknown group uid $uid");
        return -1;
    }

    private function paraRef($uid) {
        if ($uid === '' || $uid === null) return -1;
        $uid = (string)$uid;
        if (isset($this->paraOrdinal[$uid])) return $this->paraOrdinal[$uid];
        $this->warn("unknown paragraph uid $uid");
        return -1;
    }

    // -----------------------------------------------------------------------
    // Text helpers
    // -----------------------------------------------------------------------

    /**
     * Collect the <handlers> block.
     *
     * It is a sibling of the <article> elements, not their child, so it cannot
     * be found while scanning the mitril article. The block is byte-identical in
     * every game measured and the runtime reimplements it, so only the handler
     * names and a fingerprint of the whole block are kept: the fingerprint lets
     * the device notice that the platform framework changed and the conversion
     * should be redone.
     */
    private function parseHandlers($dom, $raw) {
        $handlersNodes = $dom->getElementsByTagName('handlers');
        if ($handlersNodes->length === 0) {
            $this->warn('no <handlers> block found');
            return;
        }

        $handlersNode = $handlersNodes->item(0);
        $handlerNodes = $handlersNode->getElementsByTagName('handler');
        foreach ($handlerNodes as $handlerNode) {
            $name = $handlerNode->getAttribute('name');
            if ($name !== '') $this->handlerNames[] = $name;
        }

        // Keep the historical fingerprint byte-for-byte compatible. The DOM
        // supplies the element identity and handler names; this small lexical
        // scanner only extracts the original inner source for the legacy hash.
        $rawInner = $this->extractRawElementContent($raw, 'handlers');
        if ($rawInner === null) {
            $this->warn('could not extract the original <handlers> source for fingerprint');
            $rawInner = $this->innerXml($handlersNode);
        }

        $this->handlerFingerprint = substr(md5($rawInner), 0, 16);
    }

    /**
     * Return the exact source between a named element's start and end tags.
     * This is used only to preserve the legacy handlers fingerprint; DOM remains
     * authoritative for all XML structure and data extraction.
     */
    private function extractRawElementContent($xml, $elementName) {
        $length = strlen($xml);
        $pos = 0;
        $depth = 0;
        $contentStart = -1;

        while (($lt = strpos($xml, '<', $pos)) !== false) {
            // Comments, CDATA and processing instructions can contain tag-like text.
            if (substr($xml, $lt, 4) === '<!--') {
                $end = strpos($xml, '-->', $lt + 4);
                if ($end === false) return null;
                $pos = $end + 3;
                continue;
            }
            if (substr($xml, $lt, 9) === '<![CDATA[') {
                $end = strpos($xml, ']]>', $lt + 9);
                if ($end === false) return null;
                $pos = $end + 3;
                continue;
            }
            if (substr($xml, $lt, 2) === '<?') {
                $end = strpos($xml, '?>', $lt + 2);
                if ($end === false) return null;
                $pos = $end + 2;
                continue;
            }
            if (substr($xml, $lt, 2) === '<!') {
                $end = $this->findXmlDeclarationEnd($xml, $lt + 2);
                if ($end === false) return null;
                $pos = $end + 1;
                continue;
            }

            $gt = $this->findXmlTagEnd($xml, $lt + 1);
            if ($gt === false) return null;

            $inside = trim(substr($xml, $lt + 1, $gt - $lt - 1));
            $closing = false;
            if (isset($inside[0]) && $inside[0] === '/') {
                $closing = true;
                $inside = ltrim(substr($inside, 1));
            }

            if (preg_match('/^([A-Za-z_][A-Za-z0-9_.:-]*)/', $inside, $match)) {
                $name = $match[1];
                $selfClosing = (!$closing && preg_match('/\/\s*$/', $inside));

                if ($depth === 0) {
                    if (!$closing && $name === $elementName) {
                        if ($selfClosing) return '';
                        $depth = 1;
                        $contentStart = $gt + 1;
                    }
                } else {
                    if ($name === $elementName) {
                        if ($closing) {
                            $depth--;
                            if ($depth === 0) {
                                return substr($xml, $contentStart, $lt - $contentStart);
                            }
                        } else if (!$selfClosing) {
                            $depth++;
                        }
                    }
                }
            }

            $pos = $gt + 1;
            if ($pos >= $length) break;
        }

        return null;
    }

    /** Find the closing '>' of a tag, ignoring '>' inside quoted attributes. */
    private function findXmlTagEnd($xml, $offset) {
        $length = strlen($xml);
        $quote = '';

        for ($i = $offset; $i < $length; $i++) {
            $ch = $xml[$i];
            if ($quote !== '') {
                if ($ch === $quote) $quote = '';
            } else if ($ch === '"' || $ch === "'") {
                $quote = $ch;
            } else if ($ch === '>') {
                return $i;
            }
        }

        return false;
    }

    /** Find the end of a declaration/DOCTYPE, respecting quotes and subsets. */
    private function findXmlDeclarationEnd($xml, $offset) {
        $length = strlen($xml);
        $quote = '';
        $subsetDepth = 0;

        for ($i = $offset; $i < $length; $i++) {
            $ch = $xml[$i];
            if ($quote !== '') {
                if ($ch === $quote) $quote = '';
            } else if ($ch === '"' || $ch === "'") {
                $quote = $ch;
            } else if ($ch === '[') {
                $subsetDepth++;
            } else if ($ch === ']') {
                if ($subsetDepth > 0) $subsetDepth--;
            } else if ($ch === '>' && $subsetDepth === 0) {
                return $i;
            }
        }

        return false;
    }

    /** Serialize child nodes in their original order, without the outer tag. */
    private function innerXml($node) {
        $out = '';
        foreach ($node->childNodes as $child) {
            $out .= $node->ownerDocument->saveXML($child);
        }
        return $out;
    }

    /**
     * Preserve the previous converter's entity-decoding behavior while using
     * DOM to identify the node boundaries. Nested markup remains part of text.
     */
    private function innerXmlDecoded($node) {
        return $this->decode($this->innerXml($node));
    }

    /** Convert DOM attributes to the associative shape expected by slotRef(). */
    private function domAttributes($node) {
        $out = array();
        if ($node->hasAttributes()) {
            foreach ($node->attributes as $attribute) {
                $out[$attribute->nodeName] = $attribute->nodeValue;
            }
        }
        return $out;
    }

    /**
     * Decode entities in source text.
     *
     * The charset argument is mandatory: PHP 5.3 defaults to ISO-8859-1, which
     * would corrupt every Cyrillic character.
     */
    private function decode($s) {
        return html_entity_decode($s, ENT_QUOTES, 'UTF-8');
    }

    /**
     * Escape for element content.
     *
     * ENT_NOQUOTES is correct here and keeps the text readable: a quote in
     * element content needs no escaping. Newlines and tabs are preserved by XML
     * in element content, which is exactly why prose goes here.
     */
    private function escText($s) {
        $out = htmlspecialchars($s, ENT_NOQUOTES, 'UTF-8');
        // htmlspecialchars returns '' for malformed UTF-8, which would silently
        // drop a paragraph's text. Report it instead.
        if ($out === '' && $s !== '') {
			$out = iconv('CP1251','UTF-8', $s);
			if ($out === '') {
				$this->warn('a text field is not valid UTF-8 and was emptied (value = "'.$out.'")');
			}
        }
        return $out;
    }

    /**
     * Escape for an attribute value.
     *
     * Only numbers, flags and short identifiers are ever passed here. Whitespace
     * is folded to spaces deliberately, because that is what XML itself does to
     * attribute values — doing it here keeps the output and the device's parse of
     * it in agreement instead of relying on that normalisation happening twice.
     */
    private function escAttr($s) {
        if ($s === null) return '';
        $s = preg_replace('/\s+/', ' ', $s);
        $s = preg_replace('/[\x00-\x08\x0B\x0C\x0E-\x1F]/', '', $s);
        return htmlspecialchars($s, ENT_QUOTES, 'UTF-8');
    }

    /** A comment cannot contain '--' or end with '-'. */
    private function escComment($s) {
        $s = str_replace(array('--', '>'), array('- -', '&gt;'), $s);
        return rtrim($s, '-') ;
    }

}