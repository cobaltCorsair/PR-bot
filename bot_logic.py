import sys
import time
import re
import json

from selenium import webdriver
from selenium.common.exceptions import *

from PyQt5 import QtWidgets, QtGui, QtCore, Qt
from PyQt5.QtCore import QThread

from PR_bot import Ui_MainWindow


class FileParsing:

    def __init__(self, file):
        self.file_path = file

    def get_file(self):
        with open(self.file_path, 'r') as f:
            for line in f:
                yield line.strip()


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
        topic_post = self.driver.find_element_by_xpath(".//div[contains(@class,'topicpost')]//*[contains(@class, "
                                                       "'code-box')]")
        topic_post_html = topic_post.find_elements_by_xpath("//pre")
        inner_sources = [i.get_attribute('innerHTML') for i in topic_post_html]
        self.topic_post_html = inner_sources[0]
        GetPRMessage.get_all_codes(inner_sources)
        return True

    def check_previous_pr(self, forum_url):
        """Проверка на наличие рекламы на последней странице темы"""
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
        errors = {'Не найдена форма ответа/картинка в последнем посте/код рекламы:': BotReport.NO_ELEMENTS_ERRORS,
                  'Недостоверная ссылка в теме:': BotReport.WRONG_THEME_ERRORS,
                  'Неверно найденный аккаунт:': BotReport.ACCOUNT_ERRORS,
                  'Превышено ожидание загрузки форума:': BotReport.TIMEOUT_ERRORS,
                  'Реклама уже есть на последней странице темы:': BotReport.PR_POST_HAS_ALREADY}

        for key, value in errors.items():
            if len(value) is not 0:
                print(key, value)

        print(f'Успешно пройдено форумов: {len(BotReport.SUCCESSFUL_FORUMS)} \n'
              f'Было пропущено форумов: {BotReport.get_all_errors_len()}')


class PrBot:
    """Класс пиар-бота, запускающий процесс рекламы"""

    # получаем время старта скрипта
    start_time = time.time()

    @staticmethod
    def get_work_time():
        time_export = round(round(time.time() - PrBot.start_time, 2) / 60)
        print(f'Затрачено времени на выполнение (в минутах): {time_export}')

    def __init__(self, file, ancestor_forum, ancestor_pr_topic, pr_code, mark,
                 user_login=None, user_password=None):
        # пустая переменная для текущей ссылки
        self.url = None
        # пустая переменная для id профиля
        self.user_id = None
        # список форумов из файла
        self.list_forums = file
        # ссылка на форум, который мы рекламим
        self.ancestor_forum = ancestor_forum
        # ссылка на рекламную тему на этом форме
        self.ancestor_pr_topic = ancestor_pr_topic
        # пиар-код форума, который мы рекламим
        self.pr_code = pr_code
        # маркировка Пиар-бота для сообщений на родительском форуме
        self.mark = mark
        # логин аккаунта (если есть)
        self.user_login = user_login
        # пароль аккаунта (если есть)
        self.user_password = user_password
        # подключаем драйвер
        self.chrome = Driver()

    def select_forum(self):
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
        # проходим по форумам в списке переданных ссылок
        for http in FileParsing(self.list_forums).get_file():
            self.url = http
            self.chrome.driver.switch_to.window(self.chrome.window_after)
            try:
                self.chrome.driver.get(self.url)
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
            except NoAccountMessage:
                print(f'На форуме {self.url} еще нет сообщений у этого аккаунта')
                BotReport.ACCOUNT_ERRORS.append(self.url)
            except TimeoutException:
                print(f'Ошибка загрузки форума {self.url}')
                BotReport.TIMEOUT_ERRORS.append(self.url)
            except OldPrPostCheck:
                print('Реклама уже есть на последней странице')
                BotReport.PR_POST_HAS_ALREADY.append(self.url)
            except StopIteration:
                print('Рекламная тема закончилась, необходима новая!')
                BotReport.get_bot_report()
                return False
            except JavascriptException:
                print(f'Возникли проблемы со скриптом пиар-фхода на форуме {self.url}')
                BotReport.ACCOUNT_ERRORS.append(self.url)
        else:
            BotReport.get_bot_report()
            PrBot.get_work_time()
            return True

    def go_to_forum(self):
        """Переход в рекламную тему на этом форуме"""
        p = GetPRMessage(self.chrome.driver, self.pr_code, self.mark)
        p.get_pr_code()
        if p.check_previous_pr(self.ancestor_forum):
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
                    return False
        elif results[0]:
            # проверяем логин в двойной скрипт, заходим в первый аккаунт, если False, идем во второй
            if self.try_login(logins[1]):
                if not self.get_profile_id():
                    try:
                        if self.check_guest():
                            self.forum_logout()
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
                        if self.check_guest():
                            self.forum_logout()
                        self.try_login(logins[1])
                        self.get_profile_id()
                        return True
                    except JavascriptException:
                        return False
                # если первый аккаунт озвращает False, идем во второй аккаунт
                elif not self.get_profile_id():
                    try:
                        if self.check_guest():
                            self.forum_logout()
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
    pass


class LinkError(Exception):
    pass


class NoAccountMessage(Exception):
    pass


class PartnershipTheme(Exception):
    pass


class OldPrPostCheck(Exception):
    pass


# if __name__ == '__main__':
#     test = PrBot('forums_list.txt',
#                  'https://dis.f-rpg.me',
#                  'https://dis.f-rpg.me/viewtopic.php?id=508',
#                  """[align=center][url=https://dis.f-rpg.ru/][img]https://forumstatic.ru/files/001a/e7/ed/68017.png[/img][/url]
# [url=https://dis.f-rpg.ru/viewtopic.php?id=4][b]сюжет игры[/b][/url] • [url=https://dis.f-rpg.ru/viewtopic.php?id=12][b]магические расы[/b][/url] • [url=https://dis.f-rpg.ru/viewtopic.php?id=3][b]о мире[/b][/url] • [url=https://dis.f-rpg.ru/viewtopic.php?id=8][b]организации[/b][/url]
# NC-21. Приём женских персонажей ограничен[/align]""",
#                  '', 'Ловец снов', '105105')
#     test.select_forum()


class BotWindow(QtWidgets.QMainWindow):

    def __init__(self, *args, **kwargs):
        super(BotWindow, self).__init__(*args, **kwargs)

        self.ui = Ui_MainWindow()
        self.ui.setupUi(self)

        self.forums_list = None
        self.forum_url = None
        self.pr_thread = None
        self.pr_code = None

        self.login = None
        self.password = None

        self.thread = PrBot(self.forums_list, self.forum_url, self.pr_thread, self.pr_code, self.login, self.password)

    def start_threading(self):
        pass

    #     self.ui.lineEdit_3.setPlaceholderText('Поле принимает bb-код')
    #
    #     self.urls = []
    #     self.forum_main_ui = ''
    #     self.forum_pr_topic = ''
    #     self.pr_code = ''
    #     self.bot_mark = ''
    #     # формируем новый поток для запуска
    #     self.thread = PRBot(url=None, ancestor_forum=None, ancestor_pr_topic=None, pr_code=None, mark='')
    #     self.ui.pushButton.clicked.connect(self.set_variables_to_bot)
    #     self.ui.pushButton_2.clicked.connect(self.run_bot)
    #
    # def set_variables_to_bot(self):
    #     # устанавливаем переменные для бота
    #     self.urls = self.ui.textEdit.toPlainText().replace(' ', '').split(',')
    #     self.forum_main_ui = self.ui.lineEdit.text()
    #     self.forum_pr_topic = self.ui.lineEdit_2.text()
    #     self.pr_code = self.ui.plainTextEdit.toPlainText()
    #     self.bot_mark = self.ui.lineEdit_3.text()
    #     self.ui.pushButton.setEnabled(False)
    #     self.ui.plainTextEdit.setEnabled(False)
    #     self.ui.lineEdit.setEnabled(False)
    #     self.ui.lineEdit_2.setEnabled(False)
    #     self.ui.lineEdit_3.setEnabled(False)
    #     self.ui.textEdit.setEnabled(False)
    #
    #     # передаем переменные в поток
    #     self.thread.urls = self.urls
    #     self.thread.ancestor_forum = self.forum_main_ui
    #     self.thread.ancestor_pr_topic = self.forum_pr_topic
    #     self.thread.pr_code = self.pr_code
    #     self.thread.mark = self.bot_mark
    #
    # def on_about_check_url(self, data):
    #     self.ui.progressBar.setValue(data)
    #
    # def run_bot(self):
    #     self.thread.start()
    #     self.thread.progressChanged.connect(self.on_about_check_url)


app = QtWidgets.QApplication([])
# иконка приложения
# ico = QtGui.QIcon('./icons/icon.png')
# app.setWindowIcon(ico)
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
