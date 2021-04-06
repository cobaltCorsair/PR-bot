import time
import re
import json
from selenium import webdriver
from selenium.common.exceptions import NoSuchElementException, JavascriptException


class LoginExceptions(Exception):
    pass


class LinkError(Exception):
    pass


class PrBot:
    """Класс пиар-бота, запускающий процесс рекламы"""

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
            self.driver.get(self.url)
            if self.first_enter():
                # переход в рекламную тему на этом форуме
                p = GetPRMessage(self.driver, self.pr_code, self.mark)
                p.get_pr_code()
                if p.checking_html(self.url):
                    self.driver.switch_to.window(self.window_before)
                    p.paste_pr_code()
                    p.post_to_forum()
                    p.get_post_link()
                    self.driver.switch_to.window(self.window_after)
                    p.post_pr_code_with_link()
                    p.post_to_forum()
                    return True
                else:
                    print('В шаблоне рекламы отсутствует ссылка на форум')
                    raise LinkError

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
            print('Ошибка входа')

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
                self.get_profile_id()
        elif results[0]:
            # проверяем логин в двойной скрипт, заходим в первый аккаунт, если False, идем во второй
            if self.try_login(logins[1]):
                if not self.get_profile_id():
                    try:
                        self.try_login(logins[2])
                        self.get_profile_id()
                    except JavascriptException:
                        return False
        elif results[2]:
            # если активны оба скрипта:
            if self.try_login(logins[0]):
                # если стандартный возвращает False, идем в двойной, в первый аккаунт
                if not self.get_profile_id():
                    try:
                        self.try_login(logins[1])
                        self.get_profile_id()
                    except JavascriptException:
                        return False
                # если первый аккаунт озвращает False, идем во второй аккаунт
                elif not self.get_profile_id():
                    try:
                        self.try_login(logins[2])
                        self.get_profile_id()
                    except JavascriptException:
                        return False

    def get_profile_id(self):
        """Поиск профиля на форуме"""
        # ищем профиль
        time.sleep(2)
        div_profile = self.driver.find_element_by_id('navprofile')
        profile_url = div_profile.find_element_by_css_selector('a').get_attribute('href')
        # парсим из адреса профиля текущий id залогиненного аккаунта
        user_id = profile_url.split("=")[1]
        self.user_id = user_id
        # проверка на валидность url
        self.url = self.url + '/' if self.url[-1] != '/' else self.url
        # получаем профиль рекламы
        profile_url = self.url + 'profile.php?id=' + user_id
        # переходим в профиль рекламы
        self.driver.get(profile_url)

        if self.get_pr_messages(user_id):
            return True

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
        form_answer = 'main-reply'
        xpath_code = ".//div[contains(@class,'post topicpost firstpost')]//*[contains(@class, 'code-box')]"
        xpath_image = ".//div[contains(@class,'endpost')]//*[contains(@class, 'postimg')]"

        try:
            if self.driver.find_element_by_xpath(xpath_image) and self.driver.find_element_by_xpath(
                    xpath_code) and self.driver.find_element_by_id(form_answer):
                print('Мы попали в рекламную тему на форуме ' + self.url)
                return True
        except NoSuchElementException as ex:
            print("Не найдена рекламная тема на форуме " + self.url)
            if self.url == self.ancestor_forum:
                print('Мы не смогли зайти в родительский форум, задайте ссылку вручную')
            # разлогин из аккаунта
            self.forum_logout()
            raise LoginExceptions from ex

    def forum_logout(self):
        """Разлогиниваемся из аккаунта"""
        logout_url = self.url + 'login.php?action=out&id=' + self.user_id
        self.driver.get(logout_url)


test = PrBot(['https://19centuryrussia.rusff.me', 'https://298.rusff.me', 'https://96kingdom.rusff.me',
              'https://acadia.rusff.me'], 'http://freshair.rusff.me/',
             'http://freshair.rusff.me/viewtopic.php?id=617', 'test_pr_scheme', 'test_mark')
test.select_forum()


class GetPRMessage:
    """Класс для получения изображений"""

    def __init__(self, driver, pr_code, mark=''):
        # пиар-код форума, который мы рекламим
        self.pr_code = pr_code
        # переменная для сохранения шаблона рекламы на форуме, где рекламим
        self.first_post_html = None
        # маркировка Пиар-бота для сообщений на родительском форуме
        self.mark = mark
        # переменная под ссылку на сообщение с рекламой
        self.pr_post_link = None
        # вебдрайвер
        self.driver = driver

    def get_pr_code(self):
        """Получаем шаблон рекламы на дочернем форуме"""
        first_post = self.driver.find_element_by_class_name('firstpost')
        self.first_post_html = first_post.find_element_by_xpath("//pre").get_attribute("innerHTML")
        return True

    def checking_html(self, forum_url):
        """Проверяем наличие в шаблоне ссылки на текущий форум"""
        code = self.first_post_html
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
        first_post_html_safety = re.sub(r'<span>', '', self.first_post_html).replace('</span>', '')
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
