"""Пиар-бот для ролевых форумов"""
# -*- coding: utf8 -*-
import sys
import time
import re
import json
import os

from selenium import webdriver
from selenium.common.exceptions import *

from PyQt5 import QtWidgets, QtCore, QtGui
from PyQt5.QtCore import QThread
from PyQt5.QtWidgets import QFileDialog, QMessageBox

from PR_bot import Ui_MainWindow


class FileParsing:
    """Парсим файл с адресами форумов"""
    def __init__(self, file):
        self.file_path = file
        self.size = self.get_size

    def get_file(self):
        """Открываем файл и создаем генератор для выдачи построчно"""
        with open(self.file_path) as f:
            for line in f:
                yield line.strip()

    @property
    def get_size(self):
        """Считаем количество строк в файле"""
        with open(self.file_path) as f:
            size = sum(1 for _ in f)
        return size


class GetPRMessage:
    """Класс для получения изображений"""

    def __init__(self, driver, pr_code):
        # пиар-код форума, который мы рекламим
        self.pr_code = pr_code
        # переменная для сохранения шаблона рекламы на форуме, где рекламим
        self.topic_post_html = None
        # переменная под ссылку на сообщение с рекламой
        self.pr_post_link = None
        # вебдрайвер
        self.driver = driver

    def get_pr_code(self):
        """Получаем шаблон рекламы на дочернем форуме"""
        topic_post = self.driver.find_element_by_xpath(".//div[contains(@class,'topicpost')]//*[contains(@class, "
                                                       "'code-box')]")
        topic_post_html = topic_post.find_elements_by_xpath("//pre")
        inner_sources = [i.get_attribute('innerHTML') for i in topic_post_html]
        self.topic_post_html = inner_sources[0]
        GetPRMessage.get_all_codes(inner_sources)
        return True

    def check_previous_pr(self, forum_url, check_last_page):
        """Проверка на наличие рекламы на последней странице темы"""
        if check_last_page:
            all_page_numbers = self.driver.find_elements_by_css_selector('.pagelink > a')
            url_list = [i.get_attribute('href') for i in all_page_numbers]
            next_page = self.driver.find_elements_by_css_selector('.pagelink > a.next')
            if next_page:
                last_page = url_list[-2]
                self.driver.get(last_page)
            all_post_on_page = self.driver.find_elements_by_class_name('post-content')
            inner_sources = [i.get_attribute('innerHTML') for i in all_post_on_page]
            for i in inner_sources:
                if forum_url in i:
                    return False
            return True
        else:
            return True

    @staticmethod
    def get_all_codes(inner_sources):
        """Дополнительное тестирование на то, что тема - не для заключения партнерства"""
        html_code = '&lt;/a&gt;'
        for i in inner_sources:
            if html_code in i:
                raise PartnershipTheme

    def checking_html(self, forum_url):
        """Проверяем наличие в шаблоне ссылки на текущий форум  и отсутствия тегов"""
        base_url = forum_url.split('://')[1]
        data = base_url.split('/')[0].split('.')[0]
        if data in self.topic_post_html and '&lt;/' not in self.topic_post_html:
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
        # чистим html от тегов
        first_post_html_safety = re.sub(r'<span>', '', self.topic_post_html).replace('</span>', '')
        # переписываем в json для быстрой отправки
        json_code = json.dumps(first_post_html_safety)

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


class Driver:
    """Класс драйвера для запуска автоматизации"""

    def __init__(self):
        self.options = webdriver.ChromeOptions()
        self.options.add_argument('--blink-settings=imagesEnabled=false')
        self.options.add_experimental_option("excludeSwitches", ["enable-logging"])
        self.options.add_argument('headless')
        self.executable_path = './driver/chromedriver.exe'
        # инициализация веб-драйвера
        self.driver = webdriver.Chrome(options=self.options, executable_path=self.executable_path)
        # инициализируем оба окна
        self.window_before = self.driver.window_handles[0]
        self.window_after = None


class BotReport:
    """Класс, коллекционирующий результаты прохода бота по форумам и выдающий отчет"""

    # глобальные переменные для неуспешных и успешных проходов
    SUCCESSFUL_FORUMS = []
    NO_ELEMENTS_ERRORS = []
    WRONG_THEME_ERRORS = []
    ACCOUNT_ERRORS = []
    TIMEOUT_ERRORS = []
    PR_POST_HAS_ALREADY = []

    @staticmethod
    def get_all_errors_len():
        """Получаем сумму проигнорированных форумов"""
        result = [BotReport.NO_ELEMENTS_ERRORS, BotReport.WRONG_THEME_ERRORS,
                  BotReport.ACCOUNT_ERRORS, BotReport.TIMEOUT_ERRORS, BotReport.PR_POST_HAS_ALREADY]
        lens = map(len, result)
        return sum(lens)

    @staticmethod
    def get_bot_report():
        """Записываем ошибки в репорт"""
        errors = {'Не найдена форма ответа/картинка в последнем посте/код рекламы:': BotReport.NO_ELEMENTS_ERRORS,
                  'Недостоверная ссылка в теме:': BotReport.WRONG_THEME_ERRORS,
                  'Неверно найденный аккаунт:': BotReport.ACCOUNT_ERRORS,
                  'Ошибка загрузки форума:': BotReport.TIMEOUT_ERRORS,
                  'Реклама уже есть на последней странице темы:': BotReport.PR_POST_HAS_ALREADY}

        # TODO: Написать вывод в файл
        with open(r"log.txt", "w") as file:
            for key, value in errors.items():
                if len(value) is not 0:
                    file.write(f'{key} {value} \n')

            file.write(f'Успешно пройдено форумов: {len(BotReport.SUCCESSFUL_FORUMS)} \n'
                       f'Было пропущено форумов: {BotReport.get_all_errors_len()} \n'
                       f'{PrBot.get_work_time()}')


class PrBot(QThread):
    """Класс пиар-бота, запускающий процесс рекламы"""

    # получаем время старта скрипта
    start_time = time.time()
    # получаем сигнал для прогресс-бара
    progressChanged = QtCore.pyqtSignal(int)

    @staticmethod
    def get_work_time():
        """Получаем точное время работы скрипта"""
        time_export = round(round(time.time() - PrBot.start_time, 2) / 60)
        return f'Затрачено времени на выполнение (в минутах): {time_export}'

    def __init__(self, file, ancestor_pr_topic, pr_code,
                 user_login=None, user_password=None, check_last_page=True):
        super().__init__()
        # пустая переменная для текущей ссылки
        self.url = None
        # пустая переменная для id профиля
        self.user_id = None
        # список форумов из файла
        self.list_forums = FileParsing(file)
        # ссылка на рекламную тему на этом форме
        self.ancestor_pr_topic = ancestor_pr_topic
        # ссылка на форум, который мы рекламим
        self.ancestor_forum = self.ancestor_pr_topic.split('/viewtopic')[0]
        # пиар-код форума, который мы рекламим
        self.pr_code = pr_code
        # логин аккаунта (если есть)
        self.user_login = user_login
        # пароль аккаунта (если есть)
        self.user_password = user_password
        # подключаем проверку последней страницы на повтор
        self.check_last_page = check_last_page
        # подключаем драйвер
        self.chrome = Driver()

    def run(self):
        """Выбор форума"""
        # если вход на родительский форум успешен, то
        if self.go_to_ancestor_forum():
            # переход по форумам-потомкам
            self.choice_descendant_forum()
        else:
            print('Поиск темы на форуме не удался, осуществляем прямой переход')
            self.chrome.driver.get(self.ancestor_pr_topic)
            # переход по форумам-потомкам
            self.choice_descendant_forum()

    def choice_descendant_forum(self):
        """Выбрать дочерний форум"""
        # открыть новую пустую вкладку
        self.chrome.driver.execute_script("window.open()")
        self.chrome.window_after = self.chrome.driver.window_handles[1]
        progress = 0
        # инициализируем генератор
        forum = self.list_forums.get_file()
        # проходим по форумам в списке переданных ссылок
        for _ in range(self.list_forums.size):
            self.url = next(forum)
            progress += 100 / self.list_forums.size
            self.progressChanged.emit(round(progress))
            self.chrome.driver.switch_to.window(self.chrome.window_after)
            try:
                self.chrome.driver.get('http://' + self.url.split('://')[1])
            except (TimeoutException, UnexpectedAlertPresentException):
                print(f'Невозможно загрузить форум {self.url}')
                BotReport.TIMEOUT_ERRORS.append(self.url)
            try:
                if self.first_enter():
                    self.go_to_forum()
            except LoginExceptions:
                print(f'На форуме {self.url} нет формы ответа, кода рекламы или картинки в последнем посте темы пиара')
                BotReport.NO_ELEMENTS_ERRORS.append(self.url)
            except (NoSuchElementException, ElementClickInterceptedException,
                    ElementNotInteractableException, StaleElementReferenceException):
                print(f'На форуме {self.url} не отображаются элементы дизайна')
                BotReport.NO_ELEMENTS_ERRORS.append(self.url)
            except (LinkError, PartnershipTheme):
                print(f'Тема форума {self.url} не прошла проверку на то, что она рекламная')
                BotReport.WRONG_THEME_ERRORS.append(self.url)
            except JavascriptException:
                print(f'Возникли проблемы со скриптом пиар-фхода на форуме {self.url}')
                BotReport.ACCOUNT_ERRORS.append(self.url)
            except NoAccountMessage:
                print(f'На форуме {self.url} еще нет сообщений у этого аккаунта')
                BotReport.ACCOUNT_ERRORS.append(self.url)
            except (TimeoutException, WebDriverException):
                print(f'Ошибка загрузки форума {self.url}')
                BotReport.TIMEOUT_ERRORS.append(self.url)
            except OldPrPostCheck:
                print('Реклама уже есть на последней странице')
                BotReport.PR_POST_HAS_ALREADY.append(self.url)
            except StopIteration:
                print('Рекламная тема закончилась, необходима новая!')
                BotReport.get_bot_report()
                application.set_enabled_stat_button()
                self.chrome.driver.quit()
                return False
        else:
            BotReport.get_bot_report()
            application.set_enabled_stat_button()
            self.chrome.driver.quit()
            return False

    def go_to_forum(self):
        """Переход в рекламную тему на этом форуме"""
        p = GetPRMessage(self.chrome.driver, self.pr_code)
        p.get_pr_code()
        if p.check_previous_pr(self.ancestor_forum, self.check_last_page):
            # проверяем, есть ли наша реклама на последней странице темы
            if p.checking_html(self.url):
                self.chrome.driver.switch_to.window(self.chrome.window_before)
                # проверяем, активна ли форма ответа на родительском форуме
                try:
                    self.chrome.driver.find_element_by_xpath("//*[@id='main-reply']")
                except NoSuchElementException:
                    raise StopIteration
                else:
                    p.paste_pr_code()
                    p.post_to_forum()
                    p.get_post_link()
                    self.chrome.driver.switch_to.window(self.chrome.window_after)
                    p.post_pr_code_with_link()
                    p.post_to_forum()

                    BotReport.SUCCESSFUL_FORUMS.append(self.url)
            else:
                raise LinkError
        else:
            raise OldPrPostCheck

    def go_to_ancestor_forum(self):
        """Переход к родительскому форуму"""
        try:
            # переход на родительский форум
            self.to_start()
            # заход под рекламным аккаунтом
            self.url = self.ancestor_forum
            # переход в рекламный аккаунт на этом форуме
            if self.user_login is not None and self.user_password is not None:
                # принудительный логин в необходимый аккаунт, закомментировать, если не нужен
                self.forced_pr_login()
            else:
                # для стандартного случая (без логина и пароля)
                self.first_enter()
            return True
        except LoginExceptions:
            print('Ошибка входа в аккаунт на родительском форуме')

    def to_start(self):
        """Быстрый переход на родительский форум"""
        self.chrome.driver.get(self.ancestor_forum)

    def forced_pr_login(self):
        """скрипт для принудительного входа в конкретный аккаунт"""
        # Входим на форум при помощи формы
        forced_login = f'''
        let form = '<form id="login" method="post" action="/login.php?action=in">\
        <input type=\"hidden\" name=\"form_sent\" value="1" \>\
        <input type=\"hidden" name="redirect_url" value="" \>\
        <input type=\"text" name="req_username" maxlength="25" value="{self.user_login}"\>\
        <input type=\"password" name="req_password" maxlength="16" value="{self.user_password}"\>\
        <input type=\"submit\" class=\"button\" name=\"login\"/>\
        </form>';

        $("#navlogin").after(form);
        $("#login input[type='submit']").click();
        '''
        self.chrome.driver.execute_script(forced_login)
        self.get_profile_id()

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
            results.append(self.chrome.driver.execute_script(_))

        return results

    @staticmethod
    def all_variables():
        """Вспомогательный метод - выбирает скрипт для использования"""
        accounts = ['''PiarIn();''', '''PR['in_1']();''', '''PR['in_2']();''']
        return accounts

    def try_login(self, script):
        """Пытаемся залогиниться"""
        self.chrome.driver.execute_script(script)
        return True

    def check_guest(self):
        """Проверяем, вылогинились ли мы из аккаунта"""
        group_id = str(self.chrome.driver.execute_script("return (function() { return GroupID }())"))
        if group_id != '3':
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
                    raise LoginExceptions
        elif results[0]:
            # проверяем логин в двойной скрипт, заходим в первый аккаунт, если False, идем во второй
            if self.try_login(logins[1]):
                if not self.get_profile_id():
                    try:
                        if self.check_guest():
                            self.forum_logout()
                        self.try_login(logins[2])
                        if self.get_profile_id():
                            return True
                        else:
                            raise LoginExceptions
                    except JavascriptException:
                        raise LoginExceptions
                else:
                    return True
        elif results[2]:
            # если активны оба скрипта:
            if self.try_login(logins[0]):
                # если стандартный возвращает False, идем в двойной, в первый аккаунт
                if not self.get_profile_id():
                    try:
                        if self.check_guest():
                            self.forum_logout()
                        self.try_login(logins[1])
                        if self.get_profile_id():
                            return True
                        else:
                            raise LoginExceptions
                    except JavascriptException:
                        raise LoginExceptions
                # если первый аккаунт озвращает False, идем во второй аккаунт
                elif not self.get_profile_id():
                    try:
                        if self.check_guest():
                            self.forum_logout()
                        self.try_login(logins[2])
                        if self.get_profile_id():
                            return True
                        else:
                            raise LoginExceptions
                    except JavascriptException:
                        raise LoginExceptions
                else:
                    return True
        else:
            raise JavascriptException

    def get_profile_id(self):
        """Поиск профиля на форуме"""
        # ищем профиль
        try:
            time.sleep(3)
            user_id = str(self.chrome.driver.execute_script("return (function() { return UserID }())"))
            self.user_id = user_id
            # проверка на валидность url
            self.url = self.url + '/' if self.url[-1] != '/' else self.url
            # получаем профиль рекламы
            profile_url = self.url + 'profile.php?id=' + user_id
            # переходим в профиль рекламы
            self.chrome.driver.get(profile_url)

            if self.get_pr_messages(user_id):
                return True
        except (NoSuchElementException, JavascriptException):
            print(f'Не успел загрузиться профиль на форуме {self.url}')
            raise NoAccountMessage

    def get_pr_messages(self, user_id):
        """Ищем сообщения рекламного аккаунта"""
        # получаем ссылку на все сообщения пользователя и переходим по ней
        messages = self.url + 'search.php?action=show_user_posts&user_id=' + user_id
        self.chrome.driver.get(messages)

        if self.go_to_pr_topic():
            return True

    def go_to_pr_topic(self):
        """Переходим в тему последнего сообщения"""
        # self.driver.find_element_by_link_text('Перейти к теме')
        pr_topic = self.chrome.driver.find_element_by_xpath('//*[@id="pun-main"]/div[2]/div[1]/div/div[3]/ul/li/a')
        pr_topic_link = pr_topic.get_attribute('href')
        self.chrome.driver.get(pr_topic_link)

        if self.check_image_and_form_answer():
            return True

    def check_image_and_form_answer(self):
        """Проверка на наличие картинки в сообщении"""
        form_answer = "//*[@id='main-reply']"
        xpath_code = ".//div[contains(@class,'post topicpost')]//*[contains(@class, 'code-box')]"
        xpath_image = ".//div[contains(@class,'endpost')]//*[contains(@class, 'postimg')]"

        try:
            if self.chrome.driver.find_element_by_xpath(xpath_image) and self.chrome.driver.find_element_by_xpath(
                    xpath_code) and self.chrome.driver.find_element_by_xpath(form_answer):
                return True
        except NoSuchElementException as ex:
            if self.url == self.ancestor_forum + '/' if self.ancestor_forum[-1] != '/' else self.ancestor_forum:
                print('Мы не смогли зайти в родительский форум автоматически, необходима ссылка')
            else:
                return False
            raise LoginExceptions from ex

    def forum_logout(self):
        """Разлогиниваемся из аккаунта"""
        logout_url = self.url + 'login.php?action=out&id=' + self.user_id
        self.chrome.driver.get(logout_url)


class LoginExceptions(Exception):
    """Класс ошибки логина"""
    pass


class LinkError(Exception):
    """Класс ошибки ссылки в посте темы рекламы"""
    pass


class NoAccountMessage(Exception):
    """Класс ошибки отсутствия сообщений у аккаунта"""
    pass


class PartnershipTheme(Exception):
    """Класс ошибочно выбранной темы"""
    pass


class OldPrPostCheck(Exception):
    """Класс ошибки наличия повтора рекламы на последней странице"""
    pass


class BotWindow(QtWidgets.QMainWindow):
    """Класс, запускающий интерфейс"""

    def __init__(self, *args, **kwargs):
        super(BotWindow, self).__init__(*args, **kwargs)

        self.ui = Ui_MainWindow()
        self.ui.setupUi(self)
        self.timer = QtCore.QTimer()

        self.forums_list = None
        self.pr_thread = None
        self.pr_code = None
        self.login = None
        self.password = None
        self.check_last_page = True

        self.thread = None

        self.ui.pushButton.clicked.connect(self.search_file)
        self.ui.pushButton_2.clicked.connect(self.check_variables_and_start)
        self.ui.pushButton_3.clicked.connect(BotWindow.view_stat_window)
        self.ui.action_2.triggered.connect(self.save_settings)
        self.ui.action.triggered.connect(self.set_setting)

        self.ui.pushButton_3.setEnabled(False)

        if os.path.exists('settings.json'):
            self.ui.action.setEnabled(True)
        else:
            self.ui.action.setEnabled(False)

    def search_file(self):
        """Поиск файла со списком форумов"""
        file_name = QFileDialog.getOpenFileName(self, 'Открыть файл', "*.txt")[0]
        self.ui.lineEdit_3.setText(file_name)
        self.ui.lineEdit_3.setDisabled(True)

        if len(self.ui.lineEdit_3.text()) != 0:
            self.forums_list = self.ui.lineEdit_3.text()
            return True

    def get_login_and_password(self):
        """Проверка логина (если отмечен чекбокс)"""
        if self.ui.checkBox.isChecked():
            if len(self.ui.lineEdit_5.text()) != 0 and len(self.ui.lineEdit_4.text()) != 0:
                self.login = self.ui.lineEdit_4.text()
                self.password = self.ui.lineEdit_5.text()
            else:
                if len(self.ui.lineEdit_4.text()) == 0 or len(self.ui.lineEdit_4.text()) == 0:
                    self.ui.lineEdit_4.setStyleSheet('border: 3px solid red')
                    self.timer.singleShot(1000, lambda: self.ui.lineEdit_4.setStyleSheet(''))
                if len(self.ui.lineEdit_5.text()) == 0 or len(self.ui.lineEdit_5.text()) == 0:
                    self.ui.lineEdit_5.setStyleSheet('border: 3px solid red')
                    self.timer.singleShot(1000, lambda: self.ui.lineEdit_5.setStyleSheet(''))
                return False
        return True

    def get_thread_url(self):
        """Получение ссылки на рекламную тему"""
        if len(self.ui.lineEdit.text()) != 0:
            self.pr_thread = self.ui.lineEdit.text()
            return True
        else:
            self.ui.lineEdit.setStyleSheet('border: 3px solid red')
            self.timer.singleShot(1000, lambda: self.ui.lineEdit.setStyleSheet(''))

    def check_file_list(self):
        """Проверка наличия выбранного файла со списком"""
        if len(self.ui.lineEdit_3.text()) != 0:
            return True
        else:
            self.ui.lineEdit_3.setStyleSheet('border: 3px solid red')
            self.timer.singleShot(1000, lambda: self.ui.lineEdit_3.setStyleSheet(''))

    def get_pr_code(self):
        """Получение шаблона рекламы"""
        if len(self.ui.plainTextEdit.toPlainText()) != 0:
            self.pr_code = self.ui.plainTextEdit.toPlainText()
            return True
        else:
            self.ui.plainTextEdit.setStyleSheet('border: 3px solid red')
            self.timer.singleShot(1000, lambda: self.ui.plainTextEdit.setStyleSheet(''))

    def check_pr_last_page(self):
        """Определяем, отмечен ли чекбокс проверки на повторы"""
        if self.ui.checkBox_3.isChecked():
            self.check_last_page = True
        else:
            self.check_last_page = False

    def check_variables_and_start(self):
        """Проверка всех методов на True и запуск процесса рекламы"""
        gui_methods = [self.get_login_and_password(), self.get_thread_url(), self.get_pr_code(),
                       self.check_file_list(), self.forums_list]
        if all(gui_methods):
            self.fields_disabled()
            self.check_pr_last_page()
            try:
                self.start_threading()
            except SessionNotCreatedException:
                QMessageBox.critical(self, "Ошибка ", "<b>Устаревшая версия вебдрайвера</b><br><br>"
                                                      "Посетите страницу <a href='https://chromedriver.chromium.org/downloads'>ChromeDriver Downloads</a>  "
                                                      "и скачайте подходящую для вашего браузера версию, после чего раcпакуйте <b>chromedriver.exe</b> в папку <b>driver</b>", QMessageBox.Ok)
                print('Устаревшая версия вебдрайвера')
                app.closeAllWindows()

    def fields_disabled(self):
        """Деактивация всех полей интерфейса"""
        self.ui.plainTextEdit.setEnabled(False)
        self.ui.lineEdit.setEnabled(False)
        self.ui.lineEdit_3.setEnabled(False)
        self.ui.lineEdit_4.setEnabled(False)
        self.ui.lineEdit_5.setEnabled(False)
        self.ui.pushButton.setEnabled(False)
        self.ui.pushButton_2.setEnabled(False)
        self.ui.checkBox.setEnabled(False)
        self.ui.checkBox_3.setEnabled(False)
        self.ui.action.setEnabled(False)

    def start_threading(self):
        """Старт потока"""
        self.thread = PrBot(self.forums_list, self.pr_thread, self.pr_code,
                            self.login, self.password, self.check_last_page)
        self.thread.start()
        self.thread.progressChanged.connect(self.on_about_check_url)

    def on_about_check_url(self, data):
        """Отправка значения в статусбар"""
        self.ui.progressBar.setValue(data)

    def set_enabled_stat_button(self):
        """Делаем доступной кнопку статистики"""
        self.ui.pushButton_3.setEnabled(True)
        self.ui.pushButton_3.setStyleSheet('background-color: rgb(183, 222, 255);')

    @staticmethod
    def view_stat_window():
        """Открываем файл логов"""
        os.startfile(r'log.txt')

    def save_settings(self):
        """Сохраняем параметры настроек в JSON"""
        gui_methods = [self.get_login_and_password(), self.get_thread_url(), self.get_pr_code(),
                       self.check_file_list(), self.forums_list]
        if all(gui_methods):
            values = {
                'pr_code': self.ui.plainTextEdit.toPlainText(),
                'thread_link': self.ui.lineEdit.text(),
                'list_forums': self.ui.lineEdit_3.text(),
                'account_check': True if self.ui.checkBox.isChecked() else False,
                'login': self.ui.lineEdit_4.text() if self.ui.checkBox.isChecked() else None,
                'password': self.ui.lineEdit_5.text() if self.ui.checkBox.isChecked() else None,
                'check_repeat_state': True if self.ui.checkBox_3.isChecked() else False
            }

            with open("settings.json", "w") as write_file:
                json.dump(values, write_file)

    def set_setting(self):
        """Читаем параметры настроек из JSON"""
        setting_file = 'settings.json'

        if os.path.exists(setting_file):
            with open(setting_file, encoding='utf-8') as f:
                settings_data = json.load(f)

            # если выбран логин в аккаунт
            if settings_data['account_check']:
                self.login = settings_data['login']
                self.password = settings_data['password']
                self.ui.lineEdit_4.setText(self.login)
                self.ui.lineEdit_5.setText(self.password)

                self.ui.checkBox.setChecked(True)

            # если выбрана проверка повторов
            if settings_data['check_repeat_state']:
                self.ui.checkBox_3.setChecked(True)
                self.check_last_page = True
            else:
                self.ui.checkBox_3.setChecked(False)
                self.check_last_page = False

            # заполняем настройку темы
            self.pr_thread = settings_data['thread_link']
            self.ui.lineEdit.setText(self.pr_thread)
            # заполняем список форумов
            self.forums_list = settings_data['list_forums']
            self.ui.lineEdit_3.setText(self.forums_list)
            # заполняем шаблон рекламы
            self.pr_code = settings_data['pr_code']
            self.ui.plainTextEdit.setPlainText(self.pr_code)


app = QtWidgets.QApplication([])
# иконка приложения
ico = QtGui.QIcon('./src/icon.png')
app.setWindowIcon(ico)
# стиль отображения интерфейса
app.setStyle("Fusion")
app.processEvents()
application = BotWindow()

# указываем заголовок окна
application.setWindowTitle("PR-Bot")
# задаем минимальный размер окна, до которого его можно ужимать
application.setMaximumSize(800, 600)
application.show()
sys.exit(app.exec())
