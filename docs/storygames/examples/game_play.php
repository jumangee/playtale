<?php
	if (!defined('IN_PHPBB')) {
		header('Location: /online/');
		exit;
	}

	// !!!
	// $gamedata

	//////////////////////////////////////////////////

	$requst_mode = trim(@$request[2]);
	
	function idlog($log, $msg) {
		$fp = fopen('_'.$log.'.log', "a+");
		fputs($fp, date('H:i d.m.y') . ' ' . $msg ."\r\n");
		fclose($fp);
	}

	switch ($requst_mode) {
		case 'test': {
			$game_id = intval( $gamedata['game_id'] );
			if (!$game_id || !gameIsMitril($gamedata)) {
				die('ERROR');
			}

			$key = trim(@$request[3]);
			if ($key != testPlayHash($game_id)) {
				die('');
			}
			
			idlog('testplay', ('#' . $game_id . ' (' . ($user_id > 0 ? $user_id : $userdata['session_id']).')'));
			
			try {
				$gamedata = compileGame($gamedata, false);
			}
			catch(Exception $e) {
				die($e->getMessage());
				$gamedata = false;
			}
			if ($gamedata) {
				header('Content-Type: text/xml; charset=' . ( @$_GET['enc1251'] == 'true' ? 'Windows-1251' : 'utf-8'));
				die($gamedata);
			}

			die('COMPILE ERROR');
		}
		case 'data': {
			header('Content-Type: text/xml; charset=' . ( @$_GET['enc1251'] == 'true' ? 'Windows-1251' : 'utf-8'));
			die(getGameData($gamedata['game_id']));
			break;
		}
		case 'playtale': {
			header('Content-Type: text/xml; charset=' . ( @$_GET['enc1251'] == 'true' ? 'Windows-1251' : 'utf-8'));
			
			require_once('playtale.php');
			die(playtaleConvert(getGameData($gamedata['game_id']), array(
				'name' => $gamedata['fullname'],
				'author' => $gamedata['author'],
				'index' => $gamedata['index_name'],
				'game_id' => $gamedata['game_id'],
				'tags' => $gamedata['tags'],
				'desc' => $gamedata['shortdesc']
			)));
			exit();
		}
		case 'libs': {      
			// locate to libs
			//array_splice($request, 0, 3);
			//die( 'Location: /online/player/' . $gamedata['player_type'] . '/libs/' . implode('/', $request) );
			//header('Location:  . implode('/', $request));
			exit();      
		}
		default: {		 
			$game_id = $gamedata['game_id'];
			if ($gamedata['player_type'] == 'standalone' || $gamedata['player_type'] == 'onlinebook') {
				//redirect
				incGameActivity( intval( $game_id ) );
				header('Location: ' . str_replace('http:','https:', getGameData($game_id)) );  // standalone URL in DATA (!)
				die();
			}

			if ($gamedata['player_type'] != 'mitril') {
				incGameActivity($game_id);
			}

			$title = defined('MITRIL_TEST') ? iconv('UTF-8', 'CP1251', $gamedata['fullname']) : $gamedata['fullname']; //iconv('UTF-8', 'CP1251', $gamedata['fullname']);
			$page_title = '' . $title . ' / Сторигейм / КвестБук';
			
			/////////////////
			// AGE
			
			if (!defined('MITRIL_TEST')) {
			
				$result = query('select age_rate from textgames_directory_age_mark where game_id=%%i and game_type=\'story\'', $gamedata['game_id']);
				if (!$result) {
					message_die(GENERAL_ERROR, 'Внутренняя ошибка запуска игры (#95343)');
				}
				$row = $db->sql_fetchrow($result);
				$age_rate = ($row && $row['age_rate']) ? intval($row['age_rate']) : 18;
				if ($age_rate > 16) {
					$user_birthday = null; //$user_id > 0 ? $userdata['user_birthday'] : false;
					if ($user_id > 0 && $userdata['user_birthday']) {
						try {
							$user_birthday = new DateTime($userdata['user_birthday']);
						} catch (Exception $e) {
							// Неверный формат даты в профиле
							$user_birthday = null;
						}
						
					} else {
						if (isset($_COOKIE['player_birthday'])) {
							$birth_date = explode('-', $_COOKIE['player_birthday']);
							if (count($birth_date) == 3) {
								$year = isset($birth_date[0]) ? (int)$birth_date[0] : 0;
								$month = isset($birth_date[1]) ? (int)$birth_date[1] : 0;
								$day = isset($birth_date[2]) ? (int)$birth_date[2] : 0;

								if ($year > ((int)date('Y')-100) && $year <= (int)date('Y') &&
									$month >= 1 && $month <= 12 &&
									$day >= 1 && $day <= 31 && 
									checkdate($month, $day, $year)) {
									try {
										$user_birthday = new DateTime(sprintf("%04d-%02d-%02d", $year, $month, $day));
									} catch (Exception $e) {
										$user_birthday = null;
									}
								}
								
							}
						}
					}

					if (!$user_birthday) {
						header('Location: /online/age-confirm/' . $gamedata['index_name']);
						exit();
					}

					$today = new DateTime();
					$age = $today->diff($user_birthday)->y;

					if ($age < $age_rate) {
						header('Location: /online/age-denied/' . $gamedata['index_name']);
						exit();
					}
				}
			}
			
			/////////////////
		
			if (defined('PLAYER_VK')) {
				$template->set_filenames(array(
					'body' => 'portal_online_vkapp_play.tpl')
				);
			} else {
				include($phpbb_root_path . 'includes/page_header.'.$phpEx);
				$template->set_filenames(array(
					'body' => 'online_tpl_' . $gamedata['player_type']  . ((USER_BETATESTER &&  $gamedata['player_type'] == 'mitril') ? '_beta' : '') . '.tpl')
				);
			}

			if ($user_id > 0) {
				$result = query('select count(t1.user_id) as thanked from
				phpbb_thanks t1
				inner join phpbb_topics t
					on t.topic_id = t1.topic_id
					and t1.post_id = t.topic_first_post_id
					and t1.user_id = %%i
				WHERE
					t1.topic_id = %%i', $user_id, $game_id);

				if (!$result || !$stats = $db->sql_fetchrow($result) ) {
					message_die(GENERAL_ERROR, 'Внутренняя ошибка чтения данных игры');
				}
			}
			
			$testPlayHash = testPlayHash($game_id);

			$template->assign_vars(array(
				'GAME_ID'		=> $game_id,
				'TITLE'			=> $title,
				'INDEX_NAME'    => $gamedata['index_name'],
				'PLAYER_NAME'	=> $user_id > 0 ? $userdata['username'] : 'Гость',
				'PLAYER_ID'		=> $user_id,
				'AUTHOR_NAME'	=> $gamedata['author_name'],
				'FULLNAME'      => $title,
				'DATA_URL'		=> defined('MITRIL_TEST') ? ('/online/'.$gamedata['index_name'] . '/test/' . $testPlayHash) : 'data/',
				'LIBS_URL'		=> '/online/player/' . $gamedata['player_type'] . '/libs/',
				'THANKED'		=> ($user_id > 0 && $stats['thanked'] == 0) ? 0 : 1,
				'STATUS'        => $gamedata['status'],
				'TESTUID'		=> $testPlayHash
			));
			
			// check newbie
			if ($user_id < 0) {
				$showNewbieDialog = true;
				$result = query('SELECT games, d.payment_id, donates
FROM
(
	SELECT session_id, game_id, count(DISTINCT game_id) games
	FROM `phpbb_gamebook_online_plays`
	where session_id = %%s
	GROUP by session_id
) p
left join donateauthor_order d
	on d.session_id = p.session_id
	and d.reason_id = %%i
    and d.reason_type = "storygame"
	and d.status = 1
left join
(
	select session_id, sum(amount) donates
	from donateauthor_order dao
	inner join donateauthor_payment dap
		on dap.order_id = dao.order_id
	where dao.session_id = %%s
) d2
on p.session_id = d2.session_id', $userdata['session_id'], $game_id, $userdata['session_id']);
				if ($result && $row = $db->sql_fetchrow($result)) {
					if ($row['games'] > 2 && intval($row['payment_id']) == 0 && $row['donates'] < 300) {
						$template->assign_block_vars('motivation', array(
							'GAMES' => intval($row['games'])
						));
						$showNewbieDialog = false;
					}
				}
				if ($showNewbieDialog) {
					$template->assign_block_vars('newbieHelpDialog', array());
				}
			}

			$template->pparse('body');

			if (!defined('PLAYER_VK')) {
				include($phpbb_root_path . 'includes/page_tail.'.$phpEx);
			}
		}
	}
?>