import time
import re
import json
from selenium import webdriver
from selenium.common.exceptions import NoSuchElementException, JavascriptException, TimeoutException, \
    UnexpectedAlertPresentException


class LoginExceptions(Exception):
    pass


class LinkError(Exception):
    pass


class NoAccountMessage(Exception):
    pass


class GetPRMessage:
    """Класс для получения изображений"""

    def __init__(self, driver, pr_code, mark=''):
        # пиар-код форума, который мы рекламим
        self.pr_code = pr_code
        # переменная для сохранения шаблона рекламы на форуме, где рекламим
        self.topic_post_html = None
        # маркировка Пиар-бота для сообщений на родительском форуме
        self.mark = mark
        # переменная под ссылку на сообщение с рекламой
        self.pr_post_link = None
        # вебдрайвер
        self.driver = driver

    def get_pr_code(self):
        """Получаем шаблон рекламы на дочернем форуме"""
        topic_post = self.driver.find_element_by_class_name('topicpost')
        self.topic_post_html = topic_post.find_element_by_xpath("//pre").get_attribute("innerHTML")
        return True

    def checking_html(self, forum_url):
        """Проверяем наличие в шаблоне ссылки на текущий форум"""
        code = self.topic_post_html
        base_url = forum_url.split('://')[1]
        data = base_url.split('/')[0]
        if data in code:
            return True

    def paste_pr_code(self):
        """Ищем форму ответа на родительском форуме и записываем шаблон с маркировкой"""
        # выключает нажатие tab на форме
        stop_enter_press = '''$(document).ready(function() {
                      $(window).keydown(function(event){
                        if(event.keyCode == 9) {
                          event.preventDefault();
                          return false;
                        }
                      });
                    });'''

        self.driver.execute_script(stop_enter_press)
        # чистим html от тегов и ставим маркировку
        first_post_html_safety = re.sub(r'<span>', '', self.topic_post_html).replace('</span>', '')
        pr_code_with_mark = f"{first_post_html_safety} {self.mark}"
        # переписываем в json для быстрой отправки
        json_code = json.dumps(pr_code_with_mark)

        self.get_json(json_code)
        return True

    def get_json(self, json_code):
        """Отправка json-кода в форму ответа"""
        enter_json = "let jsonData =" + json_code + "; $('#main-reply').text(jsonData);"
        self.driver.execute_script(enter_json)
        return True

    def post_to_forum(self):
        """Постим рекламу на форум"""
        main_reply_submit_button = self.driver.find_element_by_css_selector('input.button.submit')
        main_reply_submit_button.click()
        return True

    def get_post_link(self):
        """Получаем ссылку на отправленное сообщение"""
        self.pr_post_link = self.driver.current_url
        return True

    def post_pr_code_with_link(self):
        """Отправляем шаблон рекламы вместе со ссылкой"""
        end_scheme = f"{self.pr_code} {self.pr_post_link}"
        json_child_code = json.dumps(end_scheme)

        self.get_json(json_child_code)
        return True


class PrBot:
    """Класс пиар-бота, запускающий процесс рекламы"""
    # глобальные переменные для неуспешных и успешных проходов
    FAILED_FORUMS = []
    SUCCESSFUL_FORUMS = []
    # TODO: Добавить переменные для логов и их последующего вывода в файл
    # TODO: Отделить драйвер в отдельный класс

    start_time = time.time()

    def __init__(self, url, ancestor_forum, ancestor_pr_topic, pr_code, mark):
        # получаем время старта скрипта
        self.start_time = time.time()
        self.options = webdriver.ChromeOptions()
        self.options.add_argument('--blink-settings=imagesEnabled=false')
        # self.options.add_argument('headless')
        self.executable_path = './driver/chromedriver.exe'
        # инициализация веб-драйвера
        self.driver = webdriver.Chrome(options=self.options, executable_path=self.executable_path)
        # пустая переменная для текущей ссылки
        self.url = None
        # пустая переменная для id профиля
        self.user_id = None
        # список ссылок для прохода
        self.urls = url
        # ссылка на форум, который мы рекламим
        self.ancestor_forum = ancestor_forum
        # ссылка на рекламную тему на этом форме
        self.ancestor_pr_topic = ancestor_pr_topic
        # пиар-код форума, который мы рекламим
        self.pr_code = pr_code
        # маркировка Пиар-бота для сообщений на родительском форуме
        self.mark = mark
        # инициализируем оба окна
        self.window_before = self.driver.window_handles[0]
        self.window_after = None

    def select_forum(self):
        """Выбор форума"""
        # если вход на родительский форум успешен, то
        if self.go_to_ancestor_forum():
            # переход по форумам-потомкам
            self.choice_descendant_forum()
        else:
            print('Поиск темы на форуме не удался, осуществляем прямой переход')
            self.driver.get(self.ancestor_pr_topic)
            # переход по форумам-потомкам
            self.choice_descendant_forum()

    def choice_descendant_forum(self):
        """Выбрать дочерний форум"""
        # открыть новую пустую вкладку
        self.driver.execute_script("window.open()")
        self.window_after = self.driver.window_handles[1]
        # проходим по форумам в списке переданных ссылок
        forums = self.urls
        for http in forums:
            self.url = http
            self.driver.switch_to.window(self.window_after)
            try:
                self.driver.get(self.url)
            except (TimeoutException, UnexpectedAlertPresentException):
                print(f'Невозможно загрузить форум {self.url}')
                PrBot.FAILED_FORUMS.append(self.url)
            try:
                if self.first_enter():
                    self.go_to_forum()
            except (LoginExceptions, NoSuchElementException):
                print(f'На форуме {self.url} нет формы ответа, кода рекламы или картинки в последнем посте темы пиара')
                PrBot.FAILED_FORUMS.append(self.url)
            except LinkError:
                print(f'Тема форума {self.url} не прошла проверку на то, что она рекламная')
                PrBot.FAILED_FORUMS.append(self.url)
            except NoAccountMessage:
                print(f'На форуме {self.url} еще нет сообщений у этого аккаунта')
                PrBot.FAILED_FORUMS.append(self.url)
        else:
            print(f'Успешно пройдено форумов: {len(PrBot.SUCCESSFUL_FORUMS)} \n'
                  f'Было пропущено форумов: {len(PrBot.FAILED_FORUMS)}')
            self.get_work_time()
            return True

    def go_to_forum(self):
        """Переход в рекламную тему на этом форуме"""
        p = GetPRMessage(self.driver, self.pr_code, self.mark)
        p.get_pr_code()
        if p.checking_html(self.url):
            self.driver.switch_to.window(self.window_before)
            # проверяем, активна ли форма ответа на родительском форуме
            if self.driver.find_element_by_xpath("//*[@id='main-reply']"):
                p.paste_pr_code()
                p.post_to_forum()
                p.get_post_link()
                self.driver.switch_to.window(self.window_after)
                p.post_pr_code_with_link()
                p.post_to_forum()

                PrBot.SUCCESSFUL_FORUMS.append(self.url)
            else:
                print('Закончилась рекламная тема на родительском форуме')
                raise StopIteration
        else:
            print(f'В шаблоне рекламы отсутствует ссылка на форум {self.url}')
            raise LinkError

    def get_work_time(self):
        time_export = round(round(time.time() - PrBot.start_time, 2) / 60)
        print(f'Затрачено времени на выполнение (в минутах): {time_export}')

    def go_to_ancestor_forum(self):
        """Переход к родительскому форуму"""
        try:
            # переход на родительский форум
            self.to_start()
            # заход под рекламным аккаунтом
            self.url = self.ancestor_forum
            # переход в рекламную тему на этом форуме
            self.first_enter()
            return True
        except LoginExceptions:
            print('Ошибка входа в аккаунт на родительском форуме')

    def to_start(self):
        """Быстрый переход на родительский форум"""
        self.driver.get(self.ancestor_forum)

    def choice_pr_account(self):
        """Функция, ищущая пиар-вход"""

        # если есть скрипт для двух акков, но нет скрипта на стандартный вход
        one_pr = '''
                    return (function() {
                    if (window.hasOwnProperty("PR") == true && window.hasOwnProperty("PiarIn") == false) {
                    return true;
                    };
                    }());
                    '''
        # если имеется только стандартный пиар-вход и нет скрипта для двух акков
        two_pr = '''
                    return (function() {
                    if (window.hasOwnProperty("PiarIn") == true && window.hasOwnProperty("PR") == false) {
                    return true;
                    };
                    }());
                    '''
        # если есть и то, и другое
        three_pr = '''
                    return (function() {
                    if (window.hasOwnProperty("PiarIn") == true && window.hasOwnProperty("PR") == true) {
                    return true;
                    };
                    }());
                    '''
        all_scripts = [one_pr, two_pr, three_pr]
        results = []

        for _ in all_scripts:
            results.append(self.driver.execute_script(_))

        return results

    @staticmethod
    def all_variables():
        """Вспомогательный метод - выбирает скрипт для использования"""
        accounts = ['''PiarIn();''', '''PR['in_1']();''', '''PR['in_2']();''']
        return accounts

    def try_login(self, script):
        """Пытаемся залогиниться"""
        self.driver.execute_script(script)
        return True

    def first_enter(self):
        """Цепочка вариантов для логина на форум"""
        results = self.choice_pr_account()
        logins = PrBot.all_variables()

        if results[1]:
            # проверяем логин в стандартный скрипт, без ветвлений
            if self.try_login(logins[0]):
                if self.get_profile_id():
                    return True
                else:
                    return False
        elif results[0]:
            # проверяем логин в двойной скрипт, заходим в первый аккаунт, если False, идем во второй
            if self.try_login(logins[1]):
                if not self.get_profile_id():
                    try:
                        self.try_login(logins[2])
                        self.get_profile_id()
                        return True
                    except JavascriptException:
                        return False
                else:
                    return True
        elif results[2]:
            # если активны оба скрипта:
            if self.try_login(logins[0]):
                # если стандартный возвращает False, идем в двойной, в первый аккаунт
                if not self.get_profile_id():
                    try:
                        self.try_login(logins[1])
                        self.get_profile_id()
                        return True
                    except JavascriptException:
                        return False
                # если первый аккаунт озвращает False, идем во второй аккаунт
                elif not self.get_profile_id():
                    try:
                        self.try_login(logins[2])
                        self.get_profile_id()
                        return True
                    except JavascriptException:
                        return False
                else:
                    return True

    def get_profile_id(self):
        """Поиск профиля на форуме"""
        # ищем профиль
        try:
            time.sleep(3)
            user_id = str(self.driver.execute_script("return (function() { return UserID }())"))
            self.user_id = user_id
            # проверка на валидность url
            self.url = self.url + '/' if self.url[-1] != '/' else self.url
            # получаем профиль рекламы
            profile_url = self.url + 'profile.php?id=' + user_id
            # переходим в профиль рекламы
            self.driver.get(profile_url)

            if self.get_pr_messages(user_id):
                return True
        except (NoSuchElementException, JavascriptException):
            print(f'Не успел загрузиться профиль на форуме {self.url}')
            raise NoAccountMessage

    def get_pr_messages(self, user_id):
        """Ищем сообщения рекламного аккаунта"""
        # получаем ссылку на все сообщения пользователя и переходим по ней
        messages = self.url + 'search.php?action=show_user_posts&user_id=' + user_id
        self.driver.get(messages)

        if self.go_to_pr_topic():
            return True

    def go_to_pr_topic(self):
        """Переходим в тему последнего сообщения"""
        # self.driver.find_element_by_link_text('Перейти к теме')
        pr_topic = self.driver.find_element_by_xpath('//*[@id="pun-main"]/div[2]/div[1]/div/div[3]/ul/li/a')
        pr_topic_link = pr_topic.get_attribute('href')
        self.driver.get(pr_topic_link)

        if self.check_image_and_form_answer():
            return True

    def check_image_and_form_answer(self):
        """Проверка на наличие картинки в сообщении"""
        form_answer = "//*[@id='main-reply']"
        xpath_code = ".//div[contains(@class,'post topicpost')]//*[contains(@class, 'code-box')]"
        xpath_image = ".//div[contains(@class,'endpost')]//*[contains(@class, 'postimg')]"

        try:
            if self.driver.find_element_by_xpath(xpath_image) and self.driver.find_element_by_xpath(
                    xpath_code) and self.driver.find_element_by_xpath(form_answer):
                print('Мы попали в рекламную тему на форуме ' + self.url)
                return True
        except NoSuchElementException as ex:
            print("Не найдена рекламная тема на форуме " + self.url)
            if self.url == self.ancestor_forum:
                print('Мы не смогли зайти в родительский форум автоматически, необходима ссылка')
            else:
                self.forum_logout()
                return False
            raise LoginExceptions from ex

    def forum_logout(self):
        """Разлогиниваемся из аккаунта"""
        logout_url = self.url + 'login.php?action=out&id=' + self.user_id
        self.driver.get(logout_url)


test = PrBot(['https://almarein.spybb.ru'],
             'http://freshair.rusff.me/',
             'http://freshair.rusff.me/viewtopic.php?id=617&p=28',
             '[align=center][url=http://freshair.rusff.me/][img]https://i.imgur.com/5Tx4D6F.png[/img][/url][/align]',
             '[align=right][b][size=8]PR-бот[/size][/b][/align]')
test.select_forum()
