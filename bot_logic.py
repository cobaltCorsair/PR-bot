from selenium import webdriver
from pprint import pformat
import os
import sys
import time
import re
import json

from selenium.common.exceptions import NoSuchElementException, JavascriptException


class LoginExceptions(Exception):
    pass


class PrBot:
    def __init__(self, url, ancestor_forum, ancestor_pr_topic, pr_code, mark=''):
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
        # переменная для сохранения шаблона рекламы на форуме, где рекламим
        self.first_post_html = None
        # маркировка Пиар-бота для сообщений на родительском форуме
        self.mark = mark
        # инициализируем оба окна
        self.window_before = self.driver.window_handles[0]
        self.window_after = None

    def select_forum(self):
        if self.go_to_ancestor_forum():
            # открыть новую пустую вкладку
            self.driver.execute_script("window.open()")
            self.window_after = self.driver.window_handles[1]
            # проходим по форумам в списке переданных ссылок
            forums = self.urls
            for http in forums:
                self.url = http
                self.driver.switch_to.window(self.window_after)
                self.driver.get(self.url)
                self.first_enter()
        else:
            print('Логин на родительский форум не удался, работа прекращена')

    def go_to_ancestor_forum(self):
        try:
            # переход на родительский форум
            self.driver.get(self.ancestor_forum)
            # заход под рекламным аккаунтом
            self.url = self.ancestor_forum
            self.first_enter()
            # переход в рекламную тему на этом форуме
            self.to_start()
            return True
        except LoginExceptions:
            print('Ошибка входа')

    def to_start(self):
        # переход на родительский форум
        self.driver.get(self.ancestor_forum)

    def choise_pr_account(self):
        # функция, ищущая пиар-вход

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
        accounts = ['''PiarIn();''', '''PR['in_1']();''', '''PR['in_2']();''']
        return accounts

    def try_login(self, script):
        self.driver.execute_script(script)
        return True

    def first_enter(self):
        results = self.choise_pr_account()
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
        # ищем профиль
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

        self.get_pr_messages(user_id)

    def get_pr_messages(self, user_id):
        # ищем сообщения рекламного аккаунта
        # получаем ссылку на все сообщения пользователя и переходим по ней
        messages = self.url + 'search.php?action=show_user_posts&user_id=' + user_id
        self.driver.get(messages)

        self.go_to_pr_topic()

    def go_to_pr_topic(self):
        # переходим в тему последнего сообщения
        # self.driver.find_element_by_link_text('Перейти к теме')
        pr_topic = self.driver.find_element_by_xpath('//*[@id="pun-main"]/div[2]/div[1]/div/div[3]/ul/li/a')
        pr_topic_link = pr_topic.get_attribute('href')
        self.driver.get(pr_topic_link)

        self.check_image_and_form_answer()

    def check_image_and_form_answer(self):
        # проверка на наличие картинки в сообщении
        form_answer = 'main-reply'
        xpath_code = ".//div[contains(@class,'post topicpost firstpost')]//*[contains(@class, 'code-box')]"
        xpath_image = ".//div[contains(@class,'endpost')]//*[contains(@class, 'postimg')]"

        try:
            if self.driver.find_element_by_xpath(xpath_image) and self.driver.find_element_by_xpath(
                    xpath_code) and self.driver.find_element_by_id(form_answer):
                print('Мы попали в рекламную тему на форуме ' + self.url)
                return True
        except NoSuchElementException:
            print("Не найдена рекламная тема на форуме " + self.url)
            if self.url == self.ancestor_forum:
                print('Мы не смогли зайти в родительский форум, задайте ссылку вручную')
                # тут нужна функция перехода по ссылке в рекламную тему
            # разлогин из аккаунта
            self.forum_logout()
            raise LoginExceptions

    def forum_logout(self):
        # разлогиниваемся из аккаунта
        logout_url = self.url + 'login.php?action=out&id=' + self.user_id
        self.driver.get(logout_url)


test = PrBot(['https://19centuryrussia.rusff.me', 'https://298.rusff.me', 'https://96kingdom.rusff.me',
              'https://acadia.rusff.me'], 'http://freshair.rusff.me/',
             'http://freshair.rusff.me/viewtopic.php?id=617', 'Test', 'Test')
test.select_forum()
