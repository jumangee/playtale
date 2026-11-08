<?php

if (!$_SERVER['HTTPS'] || !$_SERVER['HTTPS'] == 'on') {
	header('Location: https://quest-book.ru' . $_SERVER['REQUEST_URI']);
	exit();
}

define('IN_PHPBB', true);
define('IN_KTI', true);

error_reporting( 0 );

$phpbb_root_path = './../forum/';
include($phpbb_root_path . 'extension.inc');
include($phpbb_root_path . 'common.php');

include_once($phpbb_root_path . 'includes/bbcode.php');
include_once($phpbb_root_path . 'includes/functions_post.php');
//include_once($phpbb_root_path . 'libs/stuff/botpost.php');

require_once($phpbb_root_path . 'libs/stuff/query.php');
require($phpbb_root_path . 'libs/stuff/request.php');
require($phpbb_root_path . 'libs/stuff/array2json.php');
require($phpbb_root_path . 'libs/stuff/ajaxresult.php');
//require($phpbb_root_path . 'libs/classes/class_date.php');
require($phpbb_root_path . 'includes/functions_usergroup.php');

// Start session management
$userdata = session_pagestart($user_ip, PAGE_STORYGAMES);
init_userprefs($userdata);
if (isset($_GET['telegram']) && $_GET['telegram'] === 'true') {
	$theme = setup_style(29);
}

$user_id = $userdata['user_id'];

define('USER_BETATESTER', in_array($user_id, array()) ? true : false);	//2, , 9304 // , 	//, 7299	3114, 8594

if (USER_BETATESTER) {
	//error_reporting( E_ALL );
	require('api.php');
}
else {
	error_reporting( 0 );
	require('api.php');
}

/////////////////////////////////////////////////////////////////////////////

$request = getRequest();

/////////////////////////////////////////////////////////////////////////////

$statuses = json_decode( file_get_contents( 'statuses.json'), true );
if ($statuses == NULL) {
  die('Internal error #1');
}

/////////////////////////////////////////////////////////////////////////////

switch (@$request[1]){
	case 'vkapp': {
		require_once('vkapp.php');
		die();
	}
	case 'updaterss': {
		$result = getGameList( 'normal', 1, 'player_type,shortdesc', true );
		$items = $result['page'];
		ob_start();
		require('rss.php');
		$result = ob_get_clean();
		file_put_contents('rss.xml', $result);
		die();
	}
	case 'gamelist': {
		$mode = trim(@$request[2]);
		$page = 1;
		if ($mode == 'page') {
			$page = intval(@$request[3]);
		} else if ($mode == 'all') {
			$page = -1;
		}
		$result = getGameList( SUPERUSER ? 'moderator' : 'normal', $page, 'player_type,shortdesc', true );

		die(ajaxResult('ok', $result));
		break;
	}
	case 'getgame': { // by player or by moder to review
		$game_id = intval(@$request[2]);
		if ($game_id == 0) {
			die(ajaxResult('error', "Incorrect request!"));
		}
		$result = getGame($game_id, 'data,shortdesc,player_type', true);
		if (!$result) {
			die(ajaxResult('error', "Game not found!"));
		}

		die(ajaxResult('ok', $result));
		break;
	}
	case 'cover': {
		$indexName = trim(@$request[2]);
		if (strlen($indexName) == 0) {
			header('HTTP/1.1 400 Bad Request');
			header('Content-Type: text/plain; charset=utf-8');
			die('Incorrect request');
		}
		$res = query('select cover from phpbb_gamebook_online where index_name = %%s and status > 3 limit 1', $indexName);
		if (!$res || !($row = $db->sql_fetchrow($res))) {
			header('HTTP/1.1 404 Not Found');
			header('Content-Type: text/plain; charset=utf-8');
			die('Incorrect game');
		}
		require_once('cover_convert.php');
		$result = StorygameCoverConvert::forPlayTale(
				'/var/www/u3518421/data/www/public_html/quest-book/online/upload/' . basename($row['cover']),
				$_GET['w'],
				$_GET['h']
			);
		// forPlayTale() returns the BMP bytes themselves. A failed conversion is
		// a real 404, so the device can tell "no cover" from "a cover arrived" and
		// falls back to its own icon instead of caching a broken file.
		if ($result === false) {
			header('HTTP/1.1 404 Not Found');
			header('Content-Type: text/plain; charset=utf-8');
			die('Cover convert failed');
		}
		header('Content-Type: image/bmp');
		header('Content-Length: ' . strlen($result));
		die($result);
	}
	case 'import': {
		$game_id = intval(@$request[2]);
		if ($game_id == 0) {
			header('Location: /online/');
			exit();
		}
		$gamedata = getGameByAuthor($game_id, 'player_type,mitril_setup', true);
		if (!$gamedata) {
			header('Location: /online/');
			exit();
		}

		if (checkUserAccess($game_id, $user_id) == USER_ACCESS_NONE) {
			message_die(SECURITY_ERROR, 'Вы не можете редактировать эту игру');
		}
		/*if ($gamedata['author_id'] != $user_id) {
			message_die(SECURITY_ERROR, 'Вы не можете редактировать эту игру');
		}*/

		if (gameIsMitril($gamedata)) {
			if (!@$_POST['action']) {
				$page_title = 'Импорт';
				include($phpbb_root_path . 'includes/page_header.'.$phpEx);
				$template->assign_vars(array(
					'GAME_ID'	=> $game_id,
					'ACTION'	=> 'preview',
				));

				$template->set_filenames(array(
					'body' => 'portal_online_import.tpl')
				);
				$template->pparse('body');
				include($phpbb_root_path . 'includes/page_tail.'.$phpEx);
				die();
			}

			if (@$_POST['action'] != 'import') {
				$page_title = 'Импорт';
				include($phpbb_root_path . 'includes/page_header.'.$phpEx);

				// preview
				$text = @$_POST['text'];

				$template->assign_vars(array(
					'GAME_ID'	=> $game_id,
					'ACTION'	=> 'import',
					'TEXT'		=> $text
				));

				$parse_actions = (@$_POST['actions'] !== '');
				if (strlen($text) > 1000000) {
					die('Too much text');
				}

				$text = explode("\n", $text);
				$articles = array();
				$article = '';
				$buf = array();
				$actions = array();
				$index = array();
				foreach($text as $line) {
					$line = trim($line);
					if (strlen($line) == 0) continue;
					if (intval($line) . '' == $line) {
						//save
						if (count($buf) > 0 && $article != '') {
							$buf = implode("\n", $buf);
							$uid = createUID();
							$articles[$uid] = array($article, $buf, $actions);
							$index[$article] = $uid;
						}

						//new
						$article = $line;
						$buf = array();
						$actions = array();
						continue;
					}

					if ($parse_actions) {	// && count($buf) > 0
						preg_match("/ - ([0-9]+)/i", $line, $matches);
						if (count($matches) == 2) {
							$actions[] = array(str_replace($matches[0], '', $line), $matches[1]);
							continue;
						}
					}
					$buf[] = $line;
				}
				if ($article != '') {	//count($buf) > 0 && 
					$buf = implode("\n", $buf);
					$articles[createUID()] = array($article, $buf, $actions);
				}

				$preview = '';
				foreach($articles as $uid => $a) {
					/*$article = array($a[0], $a[1], array());
					if (count($a[2]) > 0) {
						foreach($a[2] as $action) {
							//search for goto
							$goto = @$index[$action[1]];
							if (!$goto) {
								$goto = '';
							}
							$aid = createUID();
							$article[2][] = array($action[0], $goto);
						}
					}
					$preview[$uid] = $article;*/
					$act = '';
					foreach ($a[2] as $i => $c) {
						$goto = @$index[$c[1]];
						if (!$goto) {
							$goto = '';
						} else {
							$goto = $c[1];
						}
						$act .= $c[0] . ' =&gt; ' . $goto . '<br/>';
					}
					$preview .= '<b>' . $a[0] . '</b><br/>' . $a[1] . '<br/>' . $act . '<hr/>';
				}

				$template->assign_vars(array(
					'PREVIEW'	=> nl2br(print_r($preview, true))
				));

				$template->set_filenames(array(
					'body' => 'portal_online_import.tpl')
				);

				$template->pparse('body');
				include($phpbb_root_path . 'includes/page_tail.'.$phpEx);
				die();
			}

			// do import
			$text = @$_POST['text'];
			$parse_actions = (@$_POST['actions'] !== '');
			if (strlen($text) > 1000000) {
				die('Too much text');
			}

			$text = explode("\n", $text);
			$articles = array();
			$article = '';
			$buf = array();
			$actions = array();
			$index = array();
			foreach($text as $line) {
				$line = trim($line);
				if (strlen($line) == 0) continue;
				if (intval($line) . '' == $line) {
					//save
					if (count($buf) > 0 && $article != '') {
						$buf = implode("\n", $buf);
						//print 'insert into online_mitril_articles (game_id, article_uid, article_title, article_text) values ()<br/>';
						//print 'insert into online_mitril_actions (game_id, article_uid, action_text, action_goto) values ()<br/>';
						$uid = createUID();
						$articles[$uid] = array($article, $buf, $actions);
						$index[$article] = $uid;
					}

					//new
					$article = $line;
					$buf = array();
					$actions = array();
					continue;
				}

				if ($parse_actions && count($buf) > 0) {
					preg_match("/ - ([0-9]+)/i", $line, $matches);
					if (count($matches) == 2) {
						$actions[] = array(str_replace($matches[0], '', $line), $matches[1]);
						continue;
					}
				}
				$buf[] = $line;
			}
			if (count($buf) > 0 && $article != '') {
				$buf = implode("\n", $buf);
				$articles[createUID()] = array($buf, $actions);
			}
			
			$re = '/type="defaults" name="action-style" value="(\d+)"/m';
			preg_match_all($re, $gamedata['mitril_setup'], $matches, PREG_SET_ORDER, 0);
			
			$actionStyle = count($matches) ? (intval($matches[0][1])) : 1;

			foreach($articles as $uid => $a) {
				//print 'insert into online_mitril_articles (game_id, article_uid, article_title, article_text) values ('.$game_id.', '.$uid.', '.$a[0].', '.$a[1].')<br/>';
				$result = query('insert into online_mitril_articles (game_id, article_uid, article_title, article_text) values (%%i, %%s, %%s, %%s)', $game_id, $uid, $a[0], $a[1]);
				if ($result)
					if (count($a[2]) > 0) {
						foreach($a[2] as $action) {
							//search for goto
							$goto = @$index[$action[1]];
							if (!$goto) {
								$goto = '';
							}
							//print 'insert into online_mitril_actions (game_id, article_uid, action_text, action_goto) values ('.$game_id.', '.$uid.', '.$action[0].', '.$goto.')<br/>';
							$aid = createUID();
							query('insert into online_mitril_actions (game_id, article_uid, action_uid, action_text, action_goto, action_style) values (%%i, %%s, %%s, %%s, %%s, %%i)', $game_id, $uid, $aid, $action[0], $goto, $actionStyle);
						}
					}
			}

			header('Location: /online/view/' . $gamedata['index_name'] . '/');
			die();
		}

		header('Location: /online/');
		exit();
	}
	case 'import-images': {
		$game_id = intval(@$request[2]);
		if ($game_id == 0) {
			header('Location: /online/');
			exit();
		}
		$gamedata = getGameByAuthor($game_id, 'player_type,mitril_setup', true);
		if (!$gamedata) {
			header('Location: /online/');
			exit();
		}

		if (checkUserAccess($game_id, $user_id) == USER_ACCESS_NONE) {
			message_die(SECURITY_ERROR, 'Вы не можете редактировать эту игру');
		}
	

		if (!gameIsMitril($gamedata)) {
			header('Location: /online/');
			exit();
		}

		if (!@$_POST['action'] || $_POST['action'] != 'import') {
			$page_title = 'Импорт иллюстраций';
			include($phpbb_root_path . 'includes/page_header.'.$phpEx);
			$template->assign_vars(array(
				'GAME_ID'	=> $game_id,
				//'ACTION'	=> 'preview',
			));

			$template->set_filenames(array(
				'body' => 'portal_online_import-images.tpl')
			);
			$template->pparse('body');
			include($phpbb_root_path . 'includes/page_tail.'.$phpEx);
			die();
		}
		
		$upl = $_FILES['imagearchive'];
		$file_name = $upl['name'];
		$file_in_temp = $upl['tmp_name'];

		$fsize = filesize($file_in_temp);
		if ($fsize > 1024 * 1024 * 20 || $fsize < 255) {
			die("Ошибка: файл больше 20 МБ или слишком мал");
		}

		$allowedImageExt = array('jpg', 'jpeg', 'gif', 'png');
		
		/*$file_ext = end(explode(".", $file_name));
		if (!in_array($file_ext, $ext)){
			die(ajaxResult('error', "Тип файла не поддерживается"));
		}*/
		
		$file_ext = end(explode(".", $file_name));
		if ($file_ext != 'zip'){
			die("Тип файла не поддерживается");
		}

		/*$new_name = $userdata['user_id'] . '_' . md5(time().rand().time());
		$filename = $upload_dir.$new_name.'.'.$file_ext;

		$file_to_upload = $upl['tmp_name'];
		if (!move_uploaded_file($file_to_upload, $filename)){
			die(ajaxResult('error', "Внутренняя ошибка обработки"));
		}

		chmod($filename, 0755);

		$size = GetImageSize($filename);
		if (!$size || !(
		($size) && ($size[0] > 300) 
		&& ($size[1] > 300)
		))
		{
		unlink($filename);
		die(ajaxResult('error', "Ошибка: слишком мелкая иллюстрация"));
		}

		if ($size[0] > 600 || $size[1] > 600){
		if (!resizeimаgе($filename, 600)){
		  unlink($filename);
		  die(ajaxResult('error', "Внутренняя ошибка изменения размера"));
		}
		}

		$http_url = $http_url . $new_name.'.'.$file_ext;*/

		// do import
		$zip = new ZipArchive();
		;
			
		if ($zip->open($file_in_temp) !== TRUE) {
			die('Не удалось открыть архив.');
		}
		
		$imageFiles = array();
		$articles = array();
		foreach (getMitrilArticles($game_id) as $article) {
			if (trim($article['article_img']) != '') {
				continue;
			}
			$articles[$article['article_title']] = $article['article_uid'];
		}
		
		$result = array();
		// Проходим по всем файлам в архиве
		for ($i = 0; $i < $zip->numFiles; $i++) {
			// Получаем имя файла
			$fileName = $zip->getNameIndex($i);
			// Выводим имя файла
			//echo "Файл $i: $fileName\n";
			
			$image_ext = end(explode(".", $fileName));
			if (!in_array($image_ext, $allowedImageExt)){
				$result[] = "Image " . $fileName. ' skip: type not supported';
				continue;
			}
			
			$articleName = str_replace('.' . $image_ext, '', $fileName);
			if (!isset($articles[$articleName])) {
				$result[] = "Image " . $fileName. ' skip: article not found';
				continue;
			}
			$article_uid = $articles[$articleName];
			
			/*if (trim($articles[$articleName]) != '') {
				$result[] = "Image " . $fileName. ' skip: article image already set';
				continue;
			}*/
			
			$filename = $article_uid . '_' . md5( createUID() . time() ) . '.' . $image_ext;
			$filepath = getMitrilImagePath('dir', $filename);
			
			$res = copy("zip://".realpath($file_in_temp)."#".$fileName, $filepath);
			$result[] = "Image " . $fileName. ': '. $fileName .' ('.$article_uid.') // '. print_r($res, true);
			
			$res = query('update online_mitril_articles
				set
					article_img=%%t,
					article_img_align=1,
				    updated_at=unix_timestamp()
				where
					game_id = %%i
					and article_uid = %%s',
				$filename,
				$game_id,
				$article_uid);
			if (!$res) {
				$result[] = "Image " . $fileName. ': SQL error!';
				break;
			}
		}
		// Закрываем архив
		$zip->close();
		
		print implode("<br/>\n", $result);

die('---');

		header('Location: /online/import-images/'.$game_id.'/');
		die();
	}
	case 'edit': {
		/*if ($user_id == 8594 || $user_id == 8592) {
			message_die('', 'Сам такой');
		}*/
		
		$game_id = intval(@$request[2]);
		if ($game_id == 0) {
			header('Location: /online/');
			exit();
		}
		$gamedata = getGameByAuthor($game_id, 'player_type', true);
		if (!$gamedata) {
			header('Location: /online/');
			exit();
		}

		$access = checkUserAccess($game_id, $user_id);
		if ($access == USER_ACCESS_NONE && !SUPERUSER) {
		//if ($gamedata['author_id'] != $user_id && !SUPERUSER) {
			message_die(SECURITY_ERROR, 'Вы не можете редактировать эту игру');
		}

		$page_title = 'Редактор сторигейма / КвестБук';
		if (@$request[3] === 'popup') {
			$gen_simple_header = true;
		}
		include($phpbb_root_path . 'includes/page_header.'.$phpEx);

		if (gameIsMitril($gamedata)) {
			$res = query('SELECT t.*, group_id, g.name group_name
						FROM `textgames_directory_tags` t
							INNER JOIN `textgames_directory_tag_groups` g
								ON
									g.group_id = t.group
						WHERE `group` in (2, 12)');
			if ($res) {
				$last_group = null;
				while ($row = $db->sql_fetchrow($res)) {
					$g_id = $row['group'];
					if ($g_id != $last_group) {
						$template->assign_block_vars('tag_group', array(
							'ID'	=> $g_id,
							'NAME'	=> $row['group_name']
						));
						$last_group = $g_id;
					}
					$template->assign_block_vars('tag_group.tag', array(
						'ID'	=> $row['tag_id'],
						'TITLE'		=> $row['title']
					));
				}
			}
			
			$res = query('select max(progress) progress, max(blog_id) blog_id from online_mitril_editprogress where game_id=%%i group by game_id', $game_id);
			$cur_progress_value = 0;
			$cur_progress_blog = 0;
			if ($res && $row = $db->sql_fetchrow($res)) {
				$cur_progress_value = intval($row['progress']);
				$cur_progress_blog = intval($row['blog_id']);
			}

			$template->assign_vars(array(
				'USER_ID'			=> $user_id,
				'USER_AUTHOR'		=> $access == USER_ACCESS_AUTHOR ? 1 : 0,
				'GAME_ID'			=> $game_id,
				'TEST_PLAY_HASH'	=> testPlayHash($game_id),
				'GAME_INDEX'		=> $gamedata['index_name'],
				'GAME_TITLE'		=> iconv('UTF-8','CP1251', $gamedata['fullname']),
				'IN_WINDOW'			=> $gen_simple_header,
				'PROGRESS_SET'		=> $cur_progress_value,
				'PROGRESS_BLOG'     => $cur_progress_blog
			));
			
			$template->set_filenames(array(
				'body' => 'portal_online_mitriledit'.(USER_BETATESTER ? '_beta' : '').'.tpl')
			);
			
			// check newbie
			$res = query('select game_id from phpbb_gamebook_online where status = 5 and author_id = %%i limit 1', $user_id);
			if ($res && !($row = $db->sql_fetchrow($res))) {
				$template->assign_block_vars('newbieHelpDialog', array());
			}
			
			$template->pparse('body');
			include($phpbb_root_path . 'includes/page_tail.'.$phpEx);
			die();
		}

		// default - captal editor
		header('Location: /online/captal/#game/' . $game_id);
		exit();
	}
	case 'delete': {
		$game_id = intval(@$request[2]);
		$confirm = trim( @$_POST['confirmkey'] );
		if ($game_id == 0) {
			header('Location: /online/');
			exit();
		}
		$gamedata = getGameByAuthor($game_id, '', true);
		if (!$gamedata) {
			header('Location: /online/');
			exit();
		}
		if ($gamedata['author_id'] != $user_id && !SUPERUSER) {
			message_die(GENERAL_ERROR, 'Вы не можете выполнить данное действие');
		}
		$to_confirm = hash( 'sha256', $game_id . '3f434' . $gamedata['author_id'] . 'reher6');
		if ($confirm == $to_confirm ) {
			if (isset( $_POST['confirm'])) {
				// confirmed
				try {
					deleteGame($game_id);
				}
				catch (Exception $e) {
					message_die(GENERAL_ERROR, 'Ошибка удаления! Сообщите администратору: ' . $e->getMessage());
				}
				header('Location: /online/');
			} else {
				header('Location: /online/view/' . $gamedata['index_name']);
			}
			exit();
		}

		$page_title = 'Сторигеймы / КвестБук';
		include($phpbb_root_path . 'includes/page_header.'.$phpEx);

		$hidden_fields = '<input type="hidden" name="sid" value="' . $userdata['session_id'] . '" /><input type="hidden" name="confirmkey" value="' . $to_confirm . '" />';

		$template->set_filenames(array(
			'confirm' => 'confirm_body.tpl')
		);
		$template->assign_vars(array(
			'MESSAGE_TITLE' => 'Требуется подтверждение',
			'MESSAGE_TEXT' => 'Вы уверены, что хотите безвозвратно удалить игру и все её компоненты?',

			'L_YES' => $lang['Yes'],
			'L_NO' => $lang['No'],

			'S_CONFIRM_ACTION' => '/online/delete/' . $game_id,
			'S_HIDDEN_FIELDS' => $hidden_fields)
		);

		$template->pparse('confirm');
		include($phpbb_root_path . 'includes/page_tail.'.$phpEx);

		exit();
	}
	case 'mitril': {
		//error_reporting(E_ALL);			
		if (USER_BETATESTER) {
			//require('mitril.php');  // all in one
			require('mitril_beta.php');  // all in one
		} else {
			require('mitril.php');  // all in one
		}

		die();
	}
	case 'components': {
		require('components.php');  // all in one

		die();
	}
	case 'setgamestatus': {
		if ($user_id < 0) {
			die(ajaxResult('error', "Not authorized"));
		}

		$new_status = intval($request[3]);
		if ($new_status == 0) {
			die(ajaxResult('error', "Incorrect request!"));
		}

		$gamedata = getGame(intval($request[2]), $fields = '', true);
		if (!$gamedata) {
			die(ajaxResult('error', "Game not found!"));
		}

		if ($gamedata['author_id'] != $user_id && !SUPERUSER) {
			die(ajaxResult('error', "Not authorized"));
		}

		if (!checkAllowStatusChange($gamedata['status'], $new_status)){
			die(ajaxResult('error', "Incorrect request!"));
		}

		// do update
		try {
			updateGameStatusById($gamedata['status'], $new_status);
		}
		catch(Exception $e) {
			die(ajaxResult('error', "Incorrect request!"));
		}

		die(ajaxResult('ok'));

		break;
	}
	case 'updateinfo': { //by author
		if ($user_id < 0) {
		  die(ajaxResult('error', "Not authorized"));
		}

		$gamedata = getGameByAuthor(intval($request[2]), 'player_type', true);
		if (!$gamedata) {
		  die(ajaxResult('error', "Game not found!"));
		}

		$game_id = intval( $gamedata['game_id'] );
		if (checkUserAccess($game_id, $user_id) == USER_ACCESS_NONE && !SUPERUSER) {
		  die(ajaxResult('error', "Not authorized"));
		}

		if (!gameIsMitril($gamedata)) {
			if (!checkAllowStatusChange($gamedata['status'], 1)){
			  die(ajaxResult('error', "Incorrect request!"));
			}
		}

		///

		$new_index_name = trim(iconv('UTF-8', 'CP1251', $_POST['game-index']));
		$game_name = trim(iconv('UTF-8', 'CP1251', $_POST['game-name']));
		$game_cover = trim(iconv('UTF-8', 'CP1251', $_POST['game-cover']));
		$game_desc = trim(iconv('UTF-8', 'CP1251', $_POST['game-desc']));
		$game_info = trim(iconv('UTF-8', 'CP1251', $_POST['game-info']));
		
		function removeHtmlBrackets($str) {
			return str_replace(array('&#60','&#62'), '', $str);
		}
		
		if ($game_name !== strip_tags(removeHtmlBrackets($game_name)) || $game_desc !== strip_tags(removeHtmlBrackets($game_desc))) {
			die(ajaxResult('error', 'Incorrect data format'));
		}
			
		if ($new_index_name == '' || $game_name == '' || $game_desc == '' || $game_info == '') {
		  die(ajaxResult('error', 'Required fields empty'));
		}

		if (strlen($game_desc) > 140 || strlen($new_index_name) > 140) {
		  die(ajaxResult('error', 'Game desc or game index - too much! Max 140 chars'));
		}

		if (strlen($game_info) > 32000) {
		  die(ajaxResult('error', 'Game info - too much! Max 32k chars'));
		}

		if (strpos('http://quest-book.ru/', $game_cover) != 0 || strpos('https://quest-book.ru/', $game_cover) != 0) {
		  die(ajaxResult('error', 'Only images hosted at quest-book.ru allowed! Max 550x550px'));
		}

		try {
			$id = getGameIdByIndex($new_index_name);
			if ($id && $id != $gamedata['game_id']) {
				die(ajaxResult('error', "Index name is not unique"));
			}
		}
		catch (Exception $e) {
			die(ajaxResult('error', $e->getMessage()));
		}

		// do update
		try {
			if (!gameIsMitril($gamedata)) {
				if (updateAtrilGame($gamedata['game_id'], $new_index_name, $game_name, $game_info, $game_desc, $game_cover)) {
					die(ajaxResult('ok'));
				}
			}
			else {
				if (updateMitrilGame($gamedata['game_id'], $new_index_name, $game_name, $game_info, $game_desc, $game_cover)) {
					die(ajaxResult('ok'));
				}
			}
		}
		catch (Exception $e) {
			die(ajaxResult('error', $e->getMessage()));
		}
		break;
	}
	case 'publish': { //by author
		if ($user_id < 0) {
		  die(ajaxResult('error', "Not authorized"));
		}

		$gamedata = getGame(intval($request[2]), $fields = '', true);
		if (!$gamedata) {
		  die(ajaxResult('error', "Game not found!"));
		}

		if ($gamedata['author_id'] != $user_id) {
		  die(ajaxResult('error', "Not authorized"));
		}

		if (!checkAllowStatusChange($gamedata['status'], 1)){
		  die(ajaxResult('error', "Incorrect request #1"));
		}

		$data = trim(stripcslashes(@$_POST['data']));
		if ($data == '' || strlen($data) > 4096000) {
		  die(ajaxResult('error', "Incorrect request #2"));
		}
		$data = validateXml($data);
		if (!$data) {
		  die(ajaxResult('error', "Incorrect data"));
		}

		// do update
		$res = query('update phpbb_gamebook_online set
			status = 1,
			data = %%s,
			datestamp = %%i
		  where
			game_id = %%i',
			$data,
			mktime(),
			$gamedata['game_id']);
		if (!$res) {
		  die(ajaxResult('error', "Internal error #update"));
		}

		die(ajaxResult('ok'));

		break;
	}
	case 'creategame':{ // by author
		require('game_new.php');
		die();
	}
	case 'upload_cover':{ // by author
		require('upload_cover.php');
		die();
	}
	case 'preview':
	case 'view': {
		if (USER_BETATESTER) {
			//error_reporting(E_ALL);
			require('game_view.php');
			exit();
		}
		require('game_view.php');
		exit();
	}
	case 'comment': {
		require('add_comment.php');
		exit();
	}
	case 'vote': {
		$vote = trim(@$request[2]);
		$game_id = intval(@$request[3]);
		if ($game_id == 0) {
			header('Location: /online/');
			die();
		}
		$gamedata = getGame($game_id, 'player_type', false);
		if (!$gamedata) {
			header('Location: /online/');
			die();
		}
		
		
		if ((time() - $userdata['user_regdate']) < 300) {
			message_die(GENERAL_ERROR, 'stop, error #945278');
		}
		
		if (($vote != 'like' && $vote != 'dislike') || $user_id < 1) {
			header('Location: /online/view/' . $gamedata['index_name'] . '/');
			die();
		}
		$result = query('select
			(select topic_first_post_id from phpbb_topics where topic_id = %%i) as post_id,
			(select count(t1.user_id) from
				phpbb_thanks t1
				inner join phpbb_topics t
					on t.topic_id = t1.topic_id
					and t1.post_id = t.topic_first_post_id
					and t1.user_id = %%i
			WHERE
				t1.topic_id = %%i) as thanked,
			(select count(game_id) from phpbb_gamebook_online_plays where game_id = %%i and start_date > 0 and user_id = %%i) as played',
				$game_id, $user_id, $game_id, $game_id, $user_id);
		if (!$result || !$stats = $db->sql_fetchrow($result) ) {
			message_die(GENERAL_ERROR, 'Внутренняя ошибка чтения данных игры');
		}

		if ($stats['thanked'] > 0 || $stats['played'] == 0) {
			header('Location: /online/view/' . $gamedata['index_name'] . '/');
			die();
		}

		$result = query('insert into phpbb_thanks (topic_id, post_id, user_id, type, thanks_time) values (%%i, %%i, %%i, %%i, %%i)', $game_id, $stats['post_id'], $user_id, $vote == 'like' ? 1 : -1, time());
		if (!$result) {
			mitril_log('ERROR! ' . print_r($db->sql_error(), true), 0);
		}


		$res = query('update phpbb_gamebook_online o set '.($vote == 'dislike' ? 'dis' : '').'likes = (select count(t1.user_id) from
		phpbb_thanks t1
		inner join phpbb_topics t
			on t.topic_id = t1.topic_id
			and t1.post_id = t.topic_first_post_id
            and t1.type = '. ($vote == 'like' ? '1' : '-1') .'
		WHERE
		t1.topic_id = o.game_id)
		where game_id = %%i', $game_id);
		if (!$res) {
			mitril_log('ERROR! ' . print_r($db->sql_error(), true), 0);
		}

		updatePlayerRating( $user_id );
		addPointsPrice('VOTE_STORY', $user_id);
		addPointsPrice('VOTE_STORY_AUTHOR', $gamedata['author_id']);
		
		if ($vote == 'like') {
			updateAuthorRating( $gamedata['author_id'] );
		}
		add_userNotify($gamedata['author_id'], ($vote == 'like' ? 'STORY_LIKE' : 'STORY_DISLIKE'), 'Добавлена оценка "'.($vote == 'like' ? '' : 'не ').'нравится" к сторигейму <a href="/online/view/'.$gamedata['index_name'].'/">'.CP1251($gamedata['fullname']).'</a>');

		sendUserNotifyVK($gamedata['author_id'], 'Добавлена оценка "'.($vote == 'like' ? '' : 'не ').'нравится" к сторигейму' . CP1251($gamedata['fullname']), 'https://quest-book.ru/online/view/'.$gamedata['index_name'].'/');

		header('Location: /online/view/' . $gamedata['index_name'] . '/');
		die();
	}
	case 'journey': {
		require('journey.php');
		die();
	}	
	case 'list': {
		$mode = trim( @$request[2] );
		//$page = intval( @$_GET['s'] ) == 0 ? 1 : floor( intval( @$_GET['s'] ) / GAMES_PER_PAGE );
		$page = intval( @$_GET['s'] ) == 0 ? 1 : floor( intval( @$_GET['s'] ) / GAMES_PER_PAGE + 1);

		switch ($mode) {
			case 'drafts': {
				$template->set_filenames(array(
					'body' => 'portal_online_list.tpl')
				);
// SUPERUSER : ? 'moderator'
				$gamelist = getGameList( 'drafts', $page, 'shortdesc, player_type, likes, dislikes,
						(
							select game_id from phpbb_gamebook_online_plays p where p.game_id = o.game_id and '.($user_id > 0 ? ('user_id = ' . $user_id) : ('session_id = "' . $userdata['session_id'] . '"')).' limit 1
						) as played',
						false);

				$title = ': Черновики';
				$params = '?';

				$user_rating = getUserRating($userdata['user_id']);
				$author_rating = getAuthorRating($userdata['user_id']);

				$template->assign_vars(array(
					'TITLE'			  => 'Сторигеймы-черновики',
					'UNPLAYED_TOP'	  => getGameUnplayedWithTag(232, $user_id, $userdata['session_id']),
					'UNPLAYED_NON'	  => getGameUnplayedWithTag(233, $user_id, $userdata['session_id'])
				));

				break;
			}
			case 'author': {
				$author = isset($_GET['id']) ? intval( $_GET['id'] ) : trim( @$request[3] );
				if (strlen($author) > 20) {
					message_die(SECURITY_ERROR, 'Ошибка проверки');
				}

				$template->set_filenames(array(
					'body' => 'portal_online_list_author.tpl')
				);

				if ( isset($_GET['id'] )) {
					$result = query('select * from phpbb_users where user_id=%%i', $author);
				}
				else {
					$result = query('select * from phpbb_users where username=%%s', $author);
				}
				if (!$result || !( $userinfo = $db->sql_fetchrow($result) )) {
					message_die(GENERAL_ERROR, 'Автор не найден');
				}

				$uid = $userinfo['user_id'];
				
				//SUPERUSER ? 'moderator' : 
				$gamelist = getGameList( 'author', $page, 'shortdesc, player_type, likes, dislikes,
						(
							select game_id from phpbb_gamebook_online_plays p where p.game_id = o.game_id and '.($user_id > 0 ? ('user_id = ' . $user_id) : ('session_id = "' . $userdata['session_id'] . '"')).' limit 1
						) as played,
						(select count(ticket_id) from online_mitril_tickets t where t.game_id = o.game_id and t.status = 0) tickets,
						(select
							GROUP_CONCAT(ga.achi_id SEPARATOR \',\') as gameachis
						from
							online_mitril_gameachis gas
						inner join
							online_mitril_gameachi ga
							on
								gas.achi_id = ga.achi_id
						where
							gas.game_id = o_game_id
							and
							(gas.duedate is null or gas.duedate > UNIX_TIMESTAMP())
						group by gas.game_id
					  ) gameachis,
					  (select icon from online_promogame_levels l where l.level = o.promolevel) promoicon,
					  promolevel, (
						select ect.game_id from textgames_directory_gamestory_tags ect where tag_id = 232 and o.game_id = ect.game_id
					  ) as editor_choice', false, 'o.author_id = ' . intval($author) .'
				or
				(
					o.game_id in (select game_id from online_mitril_editors e where e.user_id = '.intval($author).')
					and o.status in (1, 4, 5)
				)');

				$user_rating = getUserRating($uid);
				$author_rating = getAuthorRating($uid);

				$title = ' ' . $userinfo['username']; //. ' <small>подписчиков: ' . intval($author_rating[3]) . ($user_id > 0 && !$author_rating[2] ? (' <sup><a href="/online/mitril/author/'.$uid.'/subscribe/">подписаться</a></sup>') : '') . '</small>';
				$params = '?id=' . $uid;

				$template->assign_vars(array(
					'TITLE'			=> 'Сторигеймы: ',
					'ALLOW_SUBSCRIBE'	=> intval( $user_id > 0 && $user_id != $uid),
					'SUBS'			=> intval( $user_id > 0 && !$author_rating[2] ),
					'AUTHOR_ID'		=> $uid,
					'USERNAME'		=> $userinfo['username'],
					'SUBSCRIBERS'	=> intval($author_rating[3]),
					'USER_HASH'		=> getUserHash($userinfo['user_email']),
					'PHOTO_URL'		=> $userinfo['user_photo'],
					'LOCATION'		=> $userinfo['user_from'],
					'OCCUPATION'	=> $userinfo['user_occ'],
					'INTERESTS'		=> $userinfo['user_interests'],
					'SIGN'			=> prepare_message($userinfo['user_sig'], false, true, true, $userinfo['user_sig_bbcode_uid'] == '' ? make_bbcode_uid() : $userinfo['user_sig_bbcode_uid']),
					'L_LOCATION'	=> $lang['Location'],
					'L_OCCUPATION'	=> $lang['Occupation'],
					'L_INTERESTS'	=> $lang['Interests'],
					'L_SIGNATURE'	=> 'Коротко о себе',
					'UNPLAYED_TOP'	  => getGameUnplayedWithTag(232, $user_id, $userdata['session_id']),
					//'UNPLAYED_DRAFT'  => getGameUnplayedWithTag(231, $user_id, $userdata['session_id']),
					'UNPLAYED_NON'	  => getGameUnplayedWithTag(233, $user_id, $userdata['session_id'])
				));

				break;
			}
			case 'tag': {
				$tag = trim( @$request[3] );
				if ($tag == '') {
					message_die(GENERAL_ERROR, 'Тэг не указан');
				}

				$result = query('select tag_id, title from textgames_directory_tags where title=%%s', iconv('UTF-8','CP1251', $tag));
				if (!$result || !( $tag = $db->sql_fetchrow($result) )) {
					message_die(GENERAL_ERROR, 'Тэг не найден' . $t);
				}
				
				$template->set_filenames(array(
					'body' => 'portal_online_list.tpl')
				);

				$tag_id = $tag['tag_id'];
				
				query('insert into online_mitril_tags_popularity (tag_id, event_date, session_id, datestamp) values (%%i, DATE_FORMAT(NOW(), "%Y/%m/%d"), %%s, UNIX_TIMESTAMP())', $tag_id, $userdata['session_id']);
				
				$gamelist = getGameList( SUPERUSER ? 'moderator' : 'normal', $page, 'shortdesc, player_type, likes, dislikes,
						(
							select game_id from phpbb_gamebook_online_plays p where p.game_id = o.game_id and '.($user_id > 0 ? ('user_id = ' . $user_id) : ('session_id = "' . $userdata['session_id'] . '"')).' limit 1
						) as played,
						(select
							GROUP_CONCAT(ga.achi_id SEPARATOR \',\') as gameachis
						from
							online_mitril_gameachis gas
						inner join
							online_mitril_gameachi ga
							on
								gas.achi_id = ga.achi_id
						where
							gas.game_id = o_game_id
							and
							(gas.duedate is null or gas.duedate > UNIX_TIMESTAMP())
						group by gas.game_id
					  ) gameachis,
					  (select icon from online_promogame_levels l where l.level = o.promolevel) promoicon, promolevel,
						(
							select ect.game_id from textgames_directory_gamestory_tags ect where tag_id = 232 and o.game_id = ect.game_id
						) as editor_choice',
						false, 'game_id in (select t.game_id from textgames_directory_gamestory_tags t where tag_id = ' . $tag_id . ')', 0, array($tag_id));

				$title = ': ' . $tag['title'];
				$params = $tag['title'] . '/?';

				$user_rating = getUserRating($userdata['user_id']);
				$author_rating = getAuthorRating($userdata['user_id']);

				$template->assign_vars(array(
					'TITLE'			  => 'Сторигеймы: ' . $tag['title'],
					'UNPLAYED_TOP'	  => getGameUnplayedWithTag(232, $user_id, $userdata['session_id']),
					//'UNPLAYED_DRAFT'  => getGameUnplayedWithTag(231, $user_id, $userdata['session_id']),
					'UNPLAYED_NON'	  => getGameUnplayedWithTag(233, $user_id, $userdata['session_id'])
				));
				
				// stats!
				$result = query('SELECT
	daydiff, clicks, datestamp
FROM
(SELECT @num := -1) ttt,
(
	SELECT @num as daydiff, CAST(DATE_FORMAT(DATE_ADD( DATE_SUB(NOW(), INTERVAL 30 DAY) , interval @num := @num+1 day), "%Y/%m/%d") AS CHAR(12) CHARACTER SET cp1251) AS datestamp
	from phpbb_config
	HAVING DATE_ADD(DATE_SUB(NOW(), INTERVAL 30 DAY), interval @num day) < NOW()
) t1
left join
(
	SELECT
		event_date day, count(`session_id`) clicks
	FROM
		online_mitril_tags_popularity tp
	WHERE
		tag_id = %%i
		and datestamp > (UNIX_TIMESTAMP() - 86400*31)
	group by day
) t2
on t1.datestamp = t2.day
where daydiff > -1 and daydiff < 31
order by cast(daydiff as decimal(38,10)) asc', $tag_id);
				if ($result) {
					$data = array();
					while ($row = $db->sql_fetchrow($result)) {
						$data[] = array(
						  $row['datestamp'],
						  intval($row['clicks'])
						);
					}

					$template->assign_block_vars('tag_popularity', array(
						'DATA' => json_encode($data)
					));
				}

				break;
			}
			case 'where': {
				$template->set_filenames(array(
					'body' => 'portal_online_list_filter.tpl')
				);
				
				$filters = array();
				$filtersCurrent = array();
				$filterIndex = array();
				$filterCount = 0;

				// w: категория
				$filter_cats = false;
				$filter = @$_GET['t'];
				if ($filter) {
					$filter_cats = array();
					$index = array();
					foreach ($filter as $tag) {	//explode(',', $filter)
						$tag = intval($tag);
						if ($tag > 0) {
							$filter_cats[] = $tag;
							$index[$tag] = true;
							if (count($filter_cats) > 9) {
								break;
							}
						}
					}
					$filter_cats = array_unique($filter_cats);
					if (count($filter_cats)) {	
						$filters[] = '(game_id in (select t.game_id from textgames_directory_gamestory_tags t where tag_id=' . implode(')) and (game_id in (select t.game_id from textgames_directory_gamestory_tags t where tag_id=', $filter_cats) . '))';
						$filtersCurrent[] = 't[]=' . implode('&t[]=', $filter_cats);
						$filterIndex['t'] = $index;
						$filterCount += count($filter_cats);
					}
				}
				
				// w: звезды
				$filter_stars = false;
				$filter = @$_GET['r'];
				if ($filter) {
					$filter_stars = array();
					$index = array();
					foreach ($filter as $star) {	//explode(',', $filter)
						$star = intval($star);
						if ($star > 0 && $star < 6) {
							$filter_stars[$star] = true;
							$index[$star] = true;
						}
					}
					
					$filter_stars = array_keys($filter_stars);
					if (count($filter_stars)) {
						$filters[] = '(`stars` in ('.implode(',', $filter_stars).'))';
						$filtersCurrent[] = 'r[]=' . implode('&r[]=', $filter_stars);
						$filterIndex['r'] = $index;
						$filterCount += count($filter_stars);
					}
				}
				
				// w: год
				$filter_years = false;
				$filter = @$_GET['y'];
				if ($filter != '') {
					$filter_years = array();
					$index = array();
					foreach ($filter as $year) {	//explode(',', $filter)
						$year = intval($year);
						if ($year > 2015 && $year < 2035) {
							$filter_years[] = $year;
							$index[$year] = true;
						}
					}
					$filter_years = array_unique($filter_years);

					if (count($filter_years)) {
						$filters[] = '(DATE_FORMAT( from_unixtime(`publicated`), "%Y") in ('.implode(',', $filter_years).'))';
						$filtersCurrent[] = 'y[]=' . implode('&y[]=', $filter_years);
						$filterIndex['y'] = $index;
						$filterCount += count($filter_years);
					}
				}
				
				// w: played
				$filter_played = intval(trim(@$_GET['p']));
				if ($filter_played != 0) {
					$filters[] = '((select game_id from phpbb_gamebook_online_plays p where p.game_id = o.game_id and '.($user_id > 0 ? ('user_id = ' . $user_id) : ('session_id = "' . $userdata['session_id'] . '"')).' limit 1) '.($filter_played == 1 ? ' = game_id' : 'is null').')';
					$filtersCurrent[] = 'p=' . (intval($filter_played) == 1 ? 1 : 2);
					$filterCount += 1;
				}
				
				// w: rank
				$filter_rank = false;
				$filter = @$_GET['l'];
				if ($filter) {
					$filter_rank = array();
					$index = array();
					foreach ($filter as $rank) {	//explode(',', $filter)
						$rank = intval($rank);
						if ($rank > 0 && $rank < 15) {
							$filter_rank[$rank] = true;
							$index[$rank] = true;
						}
					}
					$filter_rank = array_keys($filter_rank);
					if (count($filter_rank)) {
						$filters[] = '(promolevel in ('.implode(',', $filter_rank).'))';
						$filtersCurrent[] = 'l[]=' . implode('&l[]=', $filter_rank);
						$filterIndex['l'] = $index;
						$filterCount += count($filter_rank);
					}
				}
				
				// w: time
				$timeList = array('до 5<i class="far fa-stopwatch"></i>', '5-10<i class="far fa-stopwatch"></i>', '10-20 <i class="far fa-stopwatch"></i>', '20+<i class="far fa-stopwatch"></i>');
				$timeListVal = array('avg_playtime > 0 and avg_playtime < 301', 'avg_playtime > 300 and avg_playtime < 601', 'avg_playtime > 600 and avg_playtime < 1201', 'avg_playtime > 1200');
				$filter_time = false;
				$filter = @$_GET['m'];
				if ($filter) {
					$filter_time = array();
					$index = array();
					foreach ($filter as $time) {	//explode(',', $filter)
						$time = intval($time);
						if ($time >= 0 && $time < count($timeList)) {
							$filter_time[$time] = $timeListVal[$time];
							$index[$time] = true;
						}
					}
					$filter_time = array_values($filter_time);
					if (count($filter_time)) {
						$filters[] = '(('.implode(') or (', $filter_time).'))';
						$filtersCurrent[] = 'm[]=' . implode('&m[]=', $filter_time);
						$filterIndex['m'] = $index;
						$filterCount += count($filter_time);
					}
				}
				
				///////////////////
				
				$filter = implode(' and ', $filters);
				$params = '?' . implode('&', $filtersCurrent);

				$gamelist = getGameList( SUPERUSER ? 'moderator' : 'normal', $page, 'shortdesc, player_type, likes, dislikes,
						(
							select game_id from phpbb_gamebook_online_plays p where p.game_id = o.game_id and '.($user_id > 0 ? ('user_id = ' . $user_id) : ('session_id = "' . $userdata['session_id'] . '"')).' limit 1
						) as played,
						(select
							GROUP_CONCAT(ga.achi_id SEPARATOR \',\') as gameachis
						from
							online_mitril_gameachis gas
						inner join
							online_mitril_gameachi ga
							on
								gas.achi_id = ga.achi_id
						where
							gas.game_id = o_game_id
							and
							(gas.duedate is null or gas.duedate > UNIX_TIMESTAMP())
						group by gas.game_id
					  ) gameachis,
					  (select icon from online_promogame_levels l where l.level = o.promolevel) promoicon, promolevel,
						(
							select ect.game_id from textgames_directory_gamestory_tags ect where tag_id = 232 and o.game_id = ect.game_id
						) as editor_choice',
						false, $filter, 0, $filter_cats ? $filter_cats : null);
						
				$user_rating = getUserRating($userdata['user_id']);
				$author_rating = getAuthorRating($userdata['user_id']);
				
				$title = ' (выборка)';

				$template->assign_vars(array(
					'TITLE'			  => 'Сторигеймы (выборка)',
					'FILTER_COUNT'	  => $filterCount,
					'UNPLAYED_TOP'	  => getGameUnplayedWithTag(232, $user_id, $userdata['session_id']),
					'UNPLAYED_NON'	  => getGameUnplayedWithTag(233, $user_id, $userdata['session_id'])
				));
				
				// data!
				
				// tags
				$res = query('select t.tag_id, t.title, g.color, group_id
				from 
				textgames_directory_gamestory_tags gt
					inner join
						textgames_directory_tags t
						on
							gt.tag_id = t.tag_id
					inner join
						phpbb_gamebook_online o
						on
							gt.game_id = o.game_id
							and o.status = 5
					inner join
						textgames_directory_tag_groups g
						on
							g.group_id = t.group
				group by t.tag_id 
				order by t.group asc, title');
				if ($res) {
					$index = &$filterIndex['t'];
					while ($row = $db->sql_fetchrow($res)) {
						$template->assign_block_vars('game_tag', array(
							'ID'		=> $row['tag_id'],
							'TITLE'		=> $row['title'],
							'COLOR'		=> $row['color'],
							'CHECKED'	=> $index[$row['tag_id']] ? 'checked' : ''
						));
					}
				}
				
				// stars
				for($s = 1; $s < 6; $s++) {
					$index = &$filterIndex['r'];
					$template->assign_block_vars('game_star', array(
							'ID'		=> $s,
							'CHECKED'	=> $index[$s] ? 'checked' : ''
						));
				}
				
				// years
				for($s = 2018; $s <= intval(date('Y')); $s++) {
					$index = &$filterIndex['y'];
					$template->assign_block_vars('game_year', array(
							'ID'		=> $s,
							'CHECKED'	=> $index[$s] ? 'checked' : ''
						));
				}
				
				// status
				$statusList = array('Не фильтровать', 'Сыгранные', 'Не сыгранные');
				for($s = 0; $s < count($statusList); $s++) {
					$template->assign_block_vars('game_status', array(
							'ID'		=> $s,
							'TITLE'		=> $statusList[$s],
							'CHECKED'	=> ((!$filter_played && $s == 0) || $filter_played == $s) ? 'checked' : ''
						));
				}
				
				// time
				for($s = 0; $s < count($timeList); $s++) {
					$index = &$filterIndex['m'];
					$template->assign_block_vars('game_time', array(
							'ID'		=> $s,
							'TITLE'		=> $timeList[$s],
							'CHECKED'	=> $index[$s] ? 'checked' : ''
						));
				}
				
				// levels
				for($s = 0; $s < 11; $s++) {
					$index = &$filterIndex['l'];
					$template->assign_block_vars('game_level', array(
							'ID'		=> $s,
							'CHECKED'	=> $index[$s] ? 'checked' : ''
						));
				}
				
				// end: data
				
				break;
			}
			default: {
				message_die(GENERAL_ERROR, 'Неизвестный запрос' . $mode);
			}
		}

		$gameachisList = getGameAchisList();

		$page_title = 'Сторигеймы' . $title . ' / КвестБук';
		$keywords = 'текстовые квесты,сторигеймы,сторигейм,сторигеймы,текстовый квест,интерактивные рассказы,интерактивная литература,играть онлайн,игры онлайн,текстовые квесты онлайн';
		include($phpbb_root_path . 'includes/page_header.'.$phpEx);

		$total = intval($gamelist['total']);
		$gamelist = $gamelist['page'];
		
		$pagination = generate_newpagination('/online/list/' . $mode . '/' . $params . '&s=', $total, GAMES_PER_PAGE, intval( @$_GET['s'] ), true, true);
		//$pagination = generate_newpagination('/online/list/' . $mode . '/' . $params . '&s=', $total, intval( @$_GET['s'] ), true, true); //GAMES_PER_PAGE, ($page * GAMES_PER_PAGE - GAMES_PER_PAGE)

		$template->assign_vars(array(
			//'TITLE'			  => $page_title,
			'USER_ACTIVE'	  => ($user_id > 0 && $userdata['user_active'] == 0) ? 0 : 1,
			'USER_ID'		  => intval( $user_id ),
			'RATING'		  => $user_rating[0],
			'RATING_POS'	  => $user_rating[1],
			'PLAYED_PERCENT'  => $user_rating[2],
			'AUTHOR_RATING'	  => $author_rating[0],
			'AUTHOR_POS'	  => $author_rating[1],
			'SUBSCRIBERS'	  => intval($author_rating[3])
		));

		foreach ($gamelist as $row) {
			$tags = explode(', ', $row['tags']);
			$is_draft = false;
			$tags = explode(', ', $row['tags']);
			$tag_ids = explode(', ', $row['tag_ids']);
			foreach ($tags as $id => &$tag) {
				if ($imgTags[$tag]) {
					$gameachisArr[] = array($tag, $imgTags[$tag]);
				}
				$tag = '<a href="/online/list/where/?t[]='.$tag_ids[$id].'">' . $tag . '</a>';
			}
			/*foreach ($tags as &$tag) {
				if ($tag === 'неформат') {
					$is_draft = true;
				}
				$tag = '<a href="/online/list/tag/'.urlencode(iconv('CP1251','UTF-8', $tag)).'">' . $tag . '</a>';
			}*/
			$tags = implode(', ', $tags);
			$template->assign_block_vars('game', array(
				'TOP'			  => intval($row['top']),
				'GAME_ID'         => $row['game_id'],
				'IS_AUTHOR'		  => $row['IS_AUTHOR'],
				'SUPERUSER'		  => SUPERUSER ? 1 : 0,
				'INDEX_NAME'      => $row['index_name'],
				'FULLNAME'        => $row['fullname'],
				'AUTHOR_ID'       => $row['author_id'],
				'AUTHOR'          => $row['author_name'],
				'PLAYER'          => $row['player_type'],
				'STATUS'          => $row['status'],
				'STATE_NAME'      => iconv('UTF-8','CP1251', $statuses[$row['status']]['name']),
				'RATING'          => $row['rating_current'],
				'CREATED'         => $row['create_date'],
				'UPDATED'         => $row['last_update'],
				'COVER'           => $row['cover'],
				'TAGS'			  => $tags,
				'DESC'            => $row['shortdesc'],
				'LIKES'			  => $row['likes'],
				'DISLIKES'		  => $row['dislikes'],
				'PLAYED'		  => $row['played'] > 0 ? true : false,
				'TICKETS'		  => intval($row['tickets']),
				'STARS'			  => intval($row['stars']),
				'EDITOR_CHOICE'	  => intval($row['editor_choice']) > 0
			));

			$gameachisArr = array();
			if ($row['promoicon'] != '') {
				$gameachisArr[] = array('Ранг сторигейма', $row['promoicon']);
			}

			// gameachis normalify
			$gameachis = explode(',', $row['gameachis']);
			foreach ($gameachis as $gameachi) {
				$info = $gameachisList[ intval($gameachi) ];
				if ($info) {
					$gameachisArr[] = array($info['achi_title'], $info['achi_icon']);
				}
			}
		
			if ($is_draft) {
				$gameachisArr[] = array('Неформат', '/online/images/stamps/draft.png');
			}

			foreach ($gameachisArr as $achi) {
				$template->assign_block_vars('game.achis', array(
					'TITLE' => htmlentities( $achi[0], (ENT_COMPAT | ENT_HTML401), 'cp1251'),
					'ICON'	=> $achi[1]
				));
			}

		}

		$template->pparse('body');

		include($phpbb_root_path . 'includes/page_tail.'.$phpEx);

		die();
	}
	case 'tags': {
		$result = query('select t.tag_id, g.name, t.title, t.image, games from textgames_directory_tags t 
inner join textgames_directory_tag_groups g 
on t.group = g.group_id
inner join (
    select tag_id, count(tt.game_id) games
    from 
    textgames_directory_gamestory_tags tt
    inner join phpbb_gamebook_online o
    on o.game_id = tt.game_id and o.status in (4, 5)
    group by tt.tag_id 
) tags
on tags.tag_id = t.tag_id
ORDER by g.group_id');
		if (!$result) {
			message_die(GENERAL_ERROR, 'ошибка #1' . $t);
		}

		$template->set_filenames(array(
			'body' => 'portal_online_list_tags.tpl')
		);
		
		$last_group = false;
	
		while ($row = $db->sql_fetchrow($result)) {
			if ($last_group !== $row['name']) {
				$template->assign_block_vars('group', array(
					'NAME'      => $row['name']
				));
				$last_group = $row['name'];
			}
			$template->assign_block_vars('group.tag', array(
				'URL'  		    => $row['index_name'],
				'FULLNAME'		=> $row['title'],
				'COVER'         => $row['image'],
				'GAMES'         => $row['games'],
			));
		}

		$page_title = 'Категории - Сторигеймы / КвестБук';
		$keywords = 'текстовые квесты,сторигеймы,сторигейм,сторигеймы,текстовый квест,интерактивные рассказы,интерактивная литература,играть онлайн,игры онлайн,текстовые квесты онлайн';
		include($phpbb_root_path . 'includes/page_header.'.$phpEx);

		$template->pparse('body');

		include($phpbb_root_path . 'includes/page_tail.'.$phpEx);

		die();
	}
	case 'donateauthor': {
		require_once($phpbb_root_path . 'libs/api/requesthandler.php');
		qRequestHandler::LOADER('/var/www/u3518421/data/www/public_html/quest-book/!classes/beta/classLoader.php');
		qRequestHandler::IMPORTS('donateauthor', array('common',array('sitecommon','donateauthor'),array('sitecommon','models'),array('mitril','common')));
		if (trim(@$request[2]) == 'paid') {
			qRequestHandler::START('SiteDonateAuthor', 'paid');
		}
		qRequestHandler::START('SiteDonateAuthor', 'storygame');	// storygame mode
	}
	case 'age-confirm': {
		$index_name = trim( @$request[2] );
		if ($index_name == '') {
			message_die(GENERAL_ERROR, 'Некорректная ссылка');
		}

		// search game
		$result = getGameList('tester', 1, '', false, 'index_name = "'. mysql_real_escape_string(trim($index_name)) . '"');
		if ($result['total'] != 1) {
			header('Location: /online/');
			exit();
		}
		$gamedata = $result['page'][0];

		$birth_year = intval($_POST['birth_year']);
		$birth_month = intval($_POST['birth_month']);
		$birth_day = intval($_POST['birth_day']);

		if ($birth_year > 0 && $birth_month > 0 && $birth_day > 0) {
			if (
				($birth_year >= ($current_year - 100) && $birth_year <= intval(date('Y')))
				&& ($birth_month >= 1 && $birth_month <= 12)
				&& checkdate($birth_month, $birth_day, $birth_year)
			) {
				setcookie('player_birthday', sprintf("%04d-%02d-%02d", $birth_year, $birth_month, $birth_day), time() + 86400 * 1, '/', '', false, true);
				header('Location: /online/' . $gamedata['index_name'] . '/');
				die();
			}
		}

		if ($user_id > 0 && $userdata['user_birthday']) {
			try {
				$user_birthday = new DateTime($userdata['user_birthday']);
				
				header('Location: /online/' . $gamedata['index_name'] . '/');
				die();

			} catch (Exception $e) {
			}
		}

		$page_title = 'Доступ ограничен - Сторигеймы / КвестБук';
		$keywords = 'текстовые квесты,сторигеймы,сторигейм,сторигеймы,текстовый квест,интерактивные рассказы,интерактивная литература,играть онлайн,игры онлайн,текстовые квесты онлайн';
		include($phpbb_root_path . 'includes/page_header.'.$phpEx);

		$template->set_filenames(array(
			'body' => 'portal_online_age_confirm.tpl')
		);
		$template->assign_vars(array(
			'URL'  		    => $gamedata['index_name'],
			'NAME' 		    => $gamedata['fullname'],
		));
		for ($i = intval(date('Y')); $i > intval(date('Y')) - 100; $i--) {
			$template->assign_block_vars('birth_year', array(
				'NUM'    	=> $i
			));
		}
		
		$template->assign_block_vars($user_id > 0 ? 'user_registered' : 'user_unregistered', array());
		$template->pparse('body');

		include($phpbb_root_path . 'includes/page_tail.'.$phpEx);

		die();
	}
	case 'age-denied': {
		message_die(GENERAL_ERROR, 'Этот контент станет доступен, когда ты подрастешь<br/>
<br/>
По законам №436-ФЗ и №587-ФЗ мы обязаны ограничивать доступ к некоторым играм для несовершеннолетних. Это стандартная практика, которая помогает делать интернет безопаснее.<br/>
<br/>
Пройти игру можно будет, когда подрастешь. А пока — есть много <a href="/online/">других</a> интересных игр!');
	}
	default:  {
		if (trim(@$request[1][0]) != '' && trim(@$request[1][0]) != '?') {
			// search game
			$result = getGameList((SUPERUSER && @$request[2] != 'test') ? 'moderator' : 'tester', 1, 'player_type, shortdesc', false, 'index_name = "'. mysql_real_escape_string(trim($request[1])) . '"');
			if ($result['total'] != 1) {
				header('Location: /online/');
				exit();
			}

			$gamedata = $result['page'][0];

			// bit hack - fix urls
			if (substr($_SERVER['REQUEST_URI'], -1) != '/' && count($request) == 2) {
				header('Location: /online/'.trim($request[1]).'/');
				exit();
			}
			// play
			require('game_play.php');
			exit();
		}

		//////
		
		//if ($user_id == 2) {
			//BannerViewRequest::registerView($_SERVER['REQUEST_URI']);
		//}
		
		////////
		// preload tags with images
		
		$imgTags = array();
		$res = query('select title, icon from textgames_directory_tags t where icon <> ""');
		if ($res) {
			while ($row = $db->sql_fetchrow($res)) {
				$imgTags[$row['title']] = $row['icon'];
			}
		}
		
		////////

		$page = intval( @$_GET['s'] ) == 0 ? 1 : floor( intval( @$_GET['s'] ) / GAMES_PER_PAGE + 1);

		$promoted = array();
		$promoted_ids = array();
		if ($page == 1) {
			// top
			$res = query('select
			  0 IS_AUTHOR,
			  o.game_id,
			  o.game_id o_game_id,
			  index_name,
			  fullname,
			  author_id,
			  u.username author_name,
			  status,
			  datestamp last_update,
			  rating_current,
			  t.topic_time create_date,
			  publicated_ver,
			  cover,
			  cover_size,
			  avg_playtime,
			  tags.tags,
			  tags.tag_ids,
			  (select
					GROUP_CONCAT(ga.achi_id SEPARATOR \',\') as gameachis
				from
					online_mitril_gameachis gas
				inner join
					online_mitril_gameachi ga
					on
						gas.achi_id = ga.achi_id
				where
					gas.game_id = o_game_id
					and
					(gas.duedate is null or gas.duedate > UNIX_TIMESTAMP())
				group by gas.game_id
			  ) gameachis,
			  shortdesc, player_type, likes, dislikes,
				(
					select game_id from phpbb_gamebook_online_plays p where p.game_id = o.game_id and '.($user_id > 0 ? ('user_id = ' . $user_id) : ('session_id = "' . $userdata['session_id'] . '"')).' limit 1
				) as played,
		  	  (select count(ticket_id) from online_mitril_tickets t where t.game_id = o.game_id and t.status = 0) tickets
			from
			  phpbb_gamebook_online o
				inner join phpbb_users u
				  on u.user_id = o.author_id
				left join phpbb_topics t
				  on t.topic_id = o.game_id
				left join
				(select
					gt.game_id g_id,
					GROUP_CONCAT(title SEPARATOR \', \') as tags,
					GROUP_CONCAT(t.tag_id SEPARATOR \', \') as tag_ids
				from
					textgames_directory_gamestory_tags gt
						inner join
							textgames_directory_tags t
								on
									gt.tag_id = t.tag_id'.(
										$filterTags ? (' and t.tag_id not in ('.implode(',', $filterTags).')') : '').'
				group by gt.game_id
				) tags
				on tags.g_id = o.game_id
			where
			  status = 5
			  and publicated > ' . (time() - 2678400) . '
			  and likes > 0
			  and likes > dislikes
			  and o.game_id not in (select game_id from textgames_directory_gamestory_tags where tag_id IN (231, 233))
			order by
			  likes desc,
			  dislikes asc,
			  publicated desc
			LIMIT 1');
			if ($res && $row = $db->sql_fetchrow($res)) {
				$row['top'] = 1;
				$promoted[] = $row;
				$promoted_ids[] = $row['game_id'];
			}

			// user-promoted
			$res = query('select
			  0 IS_AUTHOR,
			  o.game_id,
			  o.game_id o_game_id,
			  index_name,
			  fullname,
			  author_id,
			  u.username author_name,
			  o.status,
			  datestamp last_update,
			  rating_current,
			  t.topic_time create_date,
			  publicated_ver,
			  cover,
			  cover_size,
			  avg_playtime,
			  tags.tags,
			  tags.tag_ids,
				(select
						GROUP_CONCAT(ga.achi_id SEPARATOR \',\') as gameachis
					from
						online_mitril_gameachis gas
					inner join
						online_mitril_gameachi ga
						on
							gas.achi_id = ga.achi_id
					where
						gas.game_id = o_game_id
						and
						(gas.duedate is null or gas.duedate > UNIX_TIMESTAMP())
					group by gas.game_id
				  ) gameachis,
				stars,
			  shortdesc, player_type, likes, dislikes,
				(
					select game_id from phpbb_gamebook_online_plays p where p.game_id = o.game_id and '.($user_id > 0 ? ('user_id = ' . $user_id) : ('session_id = "' . $userdata['session_id'] . '"')).' limit 1
				) as played,
		  	  (select count(ticket_id) from online_mitril_tickets t where t.game_id = o.game_id and t.status = 0) tickets,
		  	  IF (p.promo_id IS NULL, 0, 1) is_promo,
			  promolevel,
			  l.icon as promoicon
			from
			  phpbb_gamebook_online o
				inner join phpbb_users u
				  on u.user_id = o.author_id
				left join phpbb_topics t
				  on t.topic_id = o.game_id
				left join
            		online_promogame p
            		on p.game_id = o.game_id
            		and p.status = 1
	            left join
	            	online_promogame_levels l
	            	on l.level = promolevel
				left join
				(select
					gt.game_id g_id,
					GROUP_CONCAT(title SEPARATOR \', \') as tags,
					GROUP_CONCAT(t.tag_id SEPARATOR \', \') as tag_ids
				from
					textgames_directory_gamestory_tags gt
						inner join
							textgames_directory_tags t
								on
									gt.tag_id = t.tag_id'.(
										$filterTags ? (' and t.tag_id not in ('.implode(',', $filterTags).')') : '').'
				group by gt.game_id
				) tags
				on tags.g_id = o.game_id
			where
			  o.status = 5
			  and p.status = 1
			  and o.game_id not in (select game_id from textgames_directory_gamestory_tags where tag_id IN (231, 233))
			  ' . (count($promoted_ids) ? ('and o.game_id not in ('.implode(',', $promoted_ids).')') : '') . '
			order by
			  p.startdate asc');
			while ($res && $row = $db->sql_fetchrow($res)) {
				$row['promoted'] = 1;
				array_unshift($promoted, $row);
				array_unshift($promoted_ids, $row['game_id']);
			}

		}

		$gamelist = getGameList( SUPERUSER ? 'moderator' : 'normal', $page, 'shortdesc, player_type, likes, dislikes,
			(
				select
					history
				from
				(
					select
						game_id, GROUP_CONCAT(`plays` ORDER BY day ASC SEPARATOR ", " ) as history
					from
					(
						SELECT
							game_id, UNIX_TIMESTAMP( STR_TO_DATE( DATE_FORMAT(from_unixtime(start_date),"%Y-%m-%d") , "%Y-%m-%d") ) day, count(`play_id`) plays
						FROM
							`phpbb_gamebook_online_plays`
						WHERE
							start_date > UNIX_TIMESTAMP() - 345600
							and DATE_FORMAT(from_unixtime(start_date),"%Y-%m-%d") <> DATE_FORMAT(NOW(),"%Y-%m-%d")
						group by game_id, day
						ORDER by day ASC
					) ttt
					group by game_id
				) ttt2
				where ttt2.game_id = o.game_id
			)
			as popularity,
			(
				select game_id from phpbb_gamebook_online_plays p where p.game_id = o.game_id and '.($user_id > 0 ? ('user_id = ' . $user_id) : ('session_id = "' . $userdata['session_id'] . '"')).' limit 1
			) as played,
			(
				select ect.game_id from textgames_directory_gamestory_tags ect where tag_id = 232 and o.game_id = ect.game_id
			) as editor_choice',
			false,
			'(status <> 4) and o.game_id not in (select game_id from textgames_directory_gamestory_tags where tag_id IN (231, 233)) '  .
			(
					count($promoted_ids) ? (
						'and game_id not in ('.implode(',', $promoted_ids).')'
					)
					: ''
			),
			1);	//publicated IS NULL OR  publicated > ' . (time() - 7776000) . ' and


		$pagination = generate_newpagination('/online/?s=', $gamelist['total'], GAMES_PER_PAGE, intval( @$_GET['s'] ), true, true);

		$gamelist = $gamelist['page'];

		$template->set_filenames(array(
			'body' => 'portal_online2.tpl')
		);

		$user_rating = getUserRating($user_id, $userdata['session_id']);
		$author_rating = getAuthorRating($user_id, $userdata['session_id']);

		$template->assign_vars(array(
			'RATING'		  => $user_rating[0],
			'RATING_POS'	  => $user_rating[1],
			'PLAYED_PERCENT'  => $user_rating[2],
			'AUTHOR_RATING'	  => $author_rating[0],
			'AUTHOR_POS'	  => $author_rating[1],
			'SUBSCRIBERS'	  => intval($author_rating[3]),
			'PAGE'			  => $page,
			'USER_ACTIVE'	  => ($user_id > 0 && $userdata['user_active'] == 0) ? 0 : 1,
			'USER_ID'		  => intval( $user_id ),
			'UNPLAYED_TOP'	  => getGameUnplayedWithTag(232, $user_id, $userdata['session_id']),
			//'UNPLAYED_DRAFT'  => getGameUnplayedWithTag(231, $user_id, $userdata['session_id']),
			'UNPLAYED_NON'	  => getGameUnplayedWithTag(233, $user_id, $userdata['session_id']),
		));

		if (count($promoted)) {
			foreach ($promoted as $row) {
				array_unshift($gamelist, $row);
			}
		}

		$gameachisList = getGameAchisList();

		foreach ($gamelist as $row) {
			$gameachisArr = array();

			// tags normalify
			$tags = explode(', ', $row['tags']);
			$tag_ids = explode(', ', $row['tag_ids']);
			foreach ($tags as $id => &$tag) {
				if ($imgTags[$tag]) {
					$gameachisArr[] = array($tag, $imgTags[$tag]);
				}
				$tag = '<a href="/online/list/where/?t[]='.$tag_ids[$id].'">' . $tag . '</a>';
			}
			$tags = implode(', ', $tags);

			if ($row['promoicon'] != '') {
				$gameachisArr[] = array('Ранг сторигейма', $row['promoicon']);
			}

			// gameachis normalify
			$gameachis = explode(',', $row['gameachis']);
			foreach ($gameachis as $gameachi) {
				$info = $gameachisList[ intval($gameachi) ];
				if ($info) {
					$gameachisArr[] = array($info['achi_title'], $info['achi_icon']);
				}
			}

			$pops = $row['popularity'];
			$popular = 0;
			if (strlen($pops) > 2) {
				$pops = explode(', ', $pops);
				if (count($pops) > 2) {
					$last = 0;
					foreach ($pops as $pop) {
						if ($pop > $last) {
							$popular++;
						} else {
							$popular = 0;
						}
						$last = $pop;
					}
				} else {
					$popular = '--';
				}
			}

			$template->assign_block_vars('game', array(
				'TOP'			  => intval($row['top']),
				'PROMOTED'		  => intval($row['promoted']),
				'PROMOICON'		  => $row['promoicon'],
				'GAME_ID'         => $row['game_id'],
				'IS_AUTHOR'		  => $row['IS_AUTHOR'],
				'SUPERUSER'		  => SUPERUSER ? 1 : 0,
				'INDEX_NAME'      => $row['index_name'],
				'FULLNAME'        => $row['fullname'],
				'AUTHOR_ID'       => $row['author_id'],
				'AUTHOR'          => $row['author_name'],
				'PLAYER'          => $row['player_type'],
				'STATUS'          => $row['status'],
				'STATE_NAME'      => iconv('UTF-8','CP1251', $statuses[$row['status']]['name']),
				'RATING'          => $row['rating_current'],
				'CREATED'         => $row['create_date'],
				'UPDATED'         => $row['last_update'],
				'COVER'           => urlToWWW($row['cover']),
				'TAGS'			  => $tags,
				'DESC'            => $row['shortdesc'],
				'LIKES'			  => $row['likes'],
				'DISLIKES'		  => $row['dislikes'],
				'SUBSCRIBERS'	  => $row['subscribers'],
				'POPULAR'		  => $popular > 1 ? 1 : 0,
				'PLAYED'		  => $row['played'] > 0 ? true : false,
				'TICKETS'		  => intval($row['tickets']),
				'STARS'			  => intval($row['likes']) + intval($row['dislikes']) > 5 ? intval($row['stars']) : 0,
				'AVG_PLAY'		  => $row['avg_playtime'] > 0 ? floor($row['avg_playtime'] / 60) : 0,
				'EDITOR_CHOICE'	  => intval($row['editor_choice']) > 0
				//'PROMOICON'		  => $row['promoicon']
			));

			/*if (USER_BETATESTER) {
				print $row['game_id'] .': ' . $row['popularity'] . ' // ' . $popular .'<br/>';
			}*/

			foreach ($gameachisArr as $achi) {
				$template->assign_block_vars('game.achis', array(
						'TITLE' => htmlentities( $achi[0], (ENT_COMPAT | ENT_HTML401), 'cp1251'),
						'ICON'	=> $achi[1]
					));
			}
		}

		$online =  getUsersOnlineTotal(true);
		$template->assign_vars(array(
			'ACTIVE'		=> 'list-new',
			'USERS_ONLINE'	=> intval($online[0])
		));
		if ($online && $online[0] > 0) {
			$online = $online[1];
			foreach ($online as $user) {
				$template->assign_block_vars("user_online", array(
					'ID'	=> $user['id'],
					'NAME'	=> $user['name'],
					'AV'	=> $user['av'],
					'ACTIVE'=> $user['active'],
					'GAME'	=> $user['game']
				));
			}
		}
		
		if (intval(@$_GET['s']) == 0) {
			// new comments
			$res = query('select p.post_id, p.post_created, u.username, pt.post_text, pt.bbcode_uid, o.index_name, o.fullname
			from 
				phpbb_posts p
				inner join
					phpbb_posts_text pt
					on pt.post_id = p.post_id
				inner join
					phpbb_topics t
					on t.topic_id = p.topic_id
					and t.topic_first_post_id <> p.post_id
				inner join
					phpbb_gamebook_online o
					on o.game_id = p.topic_id
					and status in (4,5)
				inner join
					phpbb_users u
					on u.user_id = p.poster_id
			where
				p.forum_id = 87
			order by p.post_created desc
			limit 6');

			if ($res) {
				require_once('../forum/libs/api/clean_bbcode_text.php');
				require_once('../forum/libs/api/gettimediff.php');
				require_once('../forum/libs/api/num_word.php');
				
				$template->assign_block_vars("new_comments", array());
				while($msg = $db->sql_fetchrow($res)) {					
					$text = clean_bbcode_text($msg['post_text'], $msg['bbcode_uid']);
					$dateDiff = getTimeDiff($msg['post_created']);
					if ($dateDiff === 0) {
						$dateDiff = 'только что';
					} else {
						list($years, $months, $days, $hours, $mins) = $dateDiff;
						if ($years > 0) {
							$days = 0;
							$mins = 0;
						}
						if ($months > 0 || $days > 0) {
							$mins = 0;
						}
						$dateDiff = ($years+$months+$days+$hours+($mins > 1 ? $mins : 0)) == 0 ? 'минуту назад' : (
									($years ? (num_word($years, array('год','года','лет'), true) . ' ') : '') . 
									($months ? (num_word($years, array('месяц','месяца','месяцев'), true) . ' ') : '') . 
									($days ? (num_word($days, array('день','дня','дней'), true) . ' ') : '') . 
									($hours ? (num_word($hours, array('час','часа','часов'), true) . ' ') : '') . 
									($mins > 1 ? (num_word($mins, array('минута','минуты','минут'), true) . ' ') : '')
								);
					}
					$template->assign_block_vars("new_comments.comment", array(
						'NAME'	=> $msg['username'],
						'TEXT'	=> $text,
						'DATE'	=> $dateDiff,
						'POST_ID' => $msg['post_id'],
						'INDEX'	=> $msg['index_name'],
						'GAME'	=> $msg['fullname']
					));
				}
			}
			
			$res = query('select sum(amount) total from donateauthor_transacts where wallet_id <> 2 and datestamp > (unix_timestamp() - 86400 * 30)');
			if ($res && $row = $db->sql_fetchrow($res)) {
				$template->assign_block_vars("new_donates", array(
					'TOTAL_AMOUNT' => round($row['total'], 2)
				));
			}

			$res = query('select t.*, username, o.fullname, o.index_name from donateauthor_transacts t left join phpbb_users u on u.user_id = t.payer_id inner join phpbb_gamebook_online o on o.game_id = t.storygame_id where wallet_id <> 2 order by t.datestamp desc limit 5');
			$total_sum = 0;
			$donates = array();
			if ($res) {
				while ($row = $db->sql_fetchrow($res)) {
					$total_sum += $row['amount'];
					$donates[] = $row;
						//'+' . $row['amount'] 'руб' . ($row['payer_id'] > 0 ? (' от ' . $row['username']) : '') . ' к сторигейму "' .iconv('UTF-8','CP1251',$row['fullname']). '"' . "\n";
				}
			}

			if ($total_sum) {
				require_once('../forum/libs/api/gettimediff.php');
				require_once('../forum/libs/api/num_word.php');

				foreach ($donates as $row) {
					$dateDiff = getTimeDiff(floatval($row['datestamp']));
					if ($dateDiff === 0) {
						$dateDiff = 'только что';
					} else {
						list($years, $months, $days, $hours, $mins) = $dateDiff;
						if ($years > 0) {
							$days = 0;
							$mins = 0;
						}
						if ($months > 0 || $days > 0) {
							$mins = 0;
						}
						$dateDiff = ($years+$months+$days+$hours+($mins > 1 ? $mins : 0)) == 0 ? 'минуту назад' : (
									($years ? (num_word($years, array('год','года','лет'), true) . ' ') : '') . 
									($months ? (num_word($years, array('месяц','месяца','месяцев'), true) . ' ') : '') . 
									($days ? (num_word($days, array('день','дня','дней'), true) . ' ') : '') . 
									($hours ? (num_word($hours, array('час','часа','часов'), true) . ' ') : '') . 
									($mins > 1 ? (num_word($mins, array('минута','минуты','минут'), true) . ' ') : '')
								);
					}

					$template->assign_block_vars("new_donates.donate", array(
						'SUM'       => $row['amount'],
						'GAME'      => $row['fullname'],
						'DATE'      => $dateDiff,
						'URL'       => $row['index_name'],
						'USER_NAME' => $row['username']
					));
				}
			}
		}

		$page_title = 'Сторигеймы / КвестБук';
		$template->assign_vars(array(
			"META" => '<link rel="apple-touch-icon" sizes="180x180" href="/online/icons/apple-touch-icon.png">
<link rel="icon" type="image/png" sizes="32x32" href="/online/icons/favicon-32x32.png">
<link rel="icon" type="image/png" sizes="16x16" href="/online/icons/favicon-16x16.png">
<link rel="manifest" href="/online/icons/site.webmanifest">
<link rel="mask-icon" href="/online/icons/safari-pinned-tab.svg" color="#5bbad5">
<link rel="shortcut icon" href="/online/icons/favicon.ico">
<meta name="apple-mobile-web-app-title" content="Сторигеймы">
<meta name="application-name" content="Сторигеймы">
<meta name="msapplication-TileColor" content="#b91d47">
<meta name="msapplication-config" content="/online/icons/browserconfig.xml">
<meta name="theme-color" content="#ffffff">
		
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black">
<meta name="apple-mobile-web-app-title" content="Сторигеймы">
'));


		// PWA worker
		//header('Service-Worker-Allowed: /');

		$metadata = array(
			'title' => 'Сторигеймы / КвестБук',
			'desc' => 'Попробуйте себя в создании сторигеймов - коротких интерактивных рассказов, в которых ход истории и концовка зависит от принятых читателем решений!',
			'img' => 'https://quest-book.ru/online/images/titles/'.rand(1,6).'.jpg' //'https://quest-book.ru/forum/images/upload/2_7def28943cf85e324ceb411333fb42cf.jpg'
		);
		$keywords = 'текстовые квесты,сторигеймы,сторигейм,сторигеймы,текстовый квест,интерактивные рассказы,интерактивная литература,играть онлайн,игры онлайн,текстовые квесты онлайн';
		include($phpbb_root_path . 'includes/page_header.'.$phpEx);

		$template->pparse('body');

		/*if ($user_id == 2) {
			print ($db->getDump());
		}*/

		include($phpbb_root_path . 'includes/page_tail.'.$phpEx);

		break;
	}
}