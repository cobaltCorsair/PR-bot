import time
import re
import json

from selenium import webdriver
from selenium.common.exceptions import *


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
        topic_post = self.driver.find_element_by_xpath(".//div[contains(@class,'topicpost')]//*[contains(@class, 'code-box')]")
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
        # self.options.add_argument('headless')
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
                  'Реклама уже есть на последней старнице темы:': BotReport.PR_POST_HAS_ALREADY}

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

    def __init__(self, url, ancestor_forum, ancestor_pr_topic, pr_code, mark):
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
        forums = self.urls
        for http in forums:
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
            except (NoSuchElementException, ElementClickInterceptedException, ElementNotInteractableException):
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
                if self.chrome.driver.find_element_by_xpath("//*[@id='main-reply']"):
                    p.paste_pr_code()
                    p.post_to_forum()
                    p.get_post_link()
                    self.chrome.driver.switch_to.window(self.chrome.window_after)
                    p.post_pr_code_with_link()
                    p.post_to_forum()

                    BotReport.SUCCESSFUL_FORUMS.append(self.url)
                else:
                    raise StopIteration
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
            # для стандартного случая, раскомментировать
            # self.first_enter()
            # принудительный логин в необходимый аккаунт, закомментировать, если не нужен
            self.forced_pr_login()
            return True
        except LoginExceptions:
            print('Ошибка входа в аккаунт на родительском форуме')

    def to_start(self):
        """Быстрый переход на родительский форум"""
        self.chrome.driver.get(self.ancestor_forum)

    def forced_pr_login(self):
        """скрипт для принудительного входа в конкретный аккаунт"""
        user_login = 'Ловец снов'
        user_password = '105105'
        # Входим на форум при помощи формы
        forced_login = f'''
        let form = '<form id="login" method="post" action="/login.php?action=in">\
        <input type=\"hidden\" name=\"form_sent\" value="1" \>\
        <input type=\"hidden" name="redirect_url" value="" \>\
        <input type=\"text" name="req_username" maxlength="25" value="{user_login}"\>\
        <input type=\"password" name="req_password" maxlength="16" value="{user_password}"\>\
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
            if self.url == self.ancestor_forum:
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


if __name__ == '__main__':
    test = PrBot([
        # "https://1984.rolbb.me",
        # "https://19centuryrussia.rusff.me",
        # "https://2028.rusff.me",
        # "https://298.rusff.me",
        # "https://acadia.rusff.me",
        # "https://aenhanse.rolka.su",
        # "https://ainhoa.anihub.ru",
        # "https://aljw.mybb.ru",
        # "https://allnewmarvel.ru",
        # "https://almarein.spybb.ru",
        # "https://andover.f-rpg.ru",
        # "https://arkhamstories.rusff.me",
        # "https://armoni.f-rpg.me",
        # "https://artishock.rusff.me",
        # "https://astep.rusff.me",
        # "https://asunai.anihub.ru",
        # "https://aukcepraha.rusff.me",
        # "https://awakening.rolebb.ru",
        # "https://bagbones.rusff.me",
        # "https://betwixtcrossover.f-rpg.ru",
        # "https://blessthismess.rusff.me",
        # "https://blindfaith.rusff.me",
        # "https://bnw.f-rpg.me",
        # "https://bombardamaxima.rusff.me",
        # "https://borgias.mybb.ru",
        # "https://bostoncrazzy.rusff.me",
        # "https://bpht.rusff.me",
        # "https://brave-world.ru",
        # "https://breakfastclub.rusff.me",
        # "https://brighton.mybb.ru",
        # "https://british.rusff.me",
        # "https://bujan.rusff.me",
        # "https://caineville.6bb.ru",
        # "https://camelot.rolbb.ru",
        # "https://capital-queen.ru",
        # "https://castlerockisland.rusff.me",
        # "https://cgeass.rusff.me",
        # "https://cgene.rusff.me",
        # "https://chaostheory.f-rpg.ru",
        # "https://chicagobynight.rusff.me",
        # "https://chocolatte.rusff.me",
        # "https://cities.rusff.me",
        # "https://closeenemy.rusff.me",
        # "https://clubofromance.rusff.me",
        # "https://codegeass.ru",
        # "https://codevein.mybb.ru",
        # "https://crising.rusff.me",
        # "https://crossfeeling.rusff.me",
        # "https://crossreturns.rusff.me",
        # "https://crossvers.rusff.me",
        # "https://cruciatuscurse.rusff.me",
        # "https://crup.rusff.me",
        # "https://curama.mybb.ru",
        # "https://cursedcreatures.f-rpg.ru",
        # "https://cwshelter.ru",
        # "https://cyrodiilfrpg.mybb.ru",
        # "https://daever.rolka.su",
        # "https://dark-fairy.ru",
        # "https://dclub.nc-21.ru",
        # "https://dgmkwr.mybb.ru",
        # "https://docnight.rusff.me",
        # "https://dragonageone.mybb.ru",
        # "https://dragonsempire.mybb.ru",
        # "https://dragonworld.f-rpg.me",
        # "https://dreamcatcher.rusff.me",
        # "https://drinkbutterbeer.rusff.me",
        # "https://eltropicanolife.rusff.me",
        # "https://enteros.rusff.me",
        # "https://essenceofblood.rusff.me",
        # "https://except-us.ru",
        # "https://exlibris.rusff.me",
        # "https://explodingsnaps.mybb.ru",
        # "https://fantalesofmarvel.rolbb.ru",
        # "https://fap.mybb.ru",
        # "https://felicis.anihub.ru",
        # "https://fern.rusff.me",
        # "https://finiteincantatem.rusff.me",
        # "https://fkme.nc-21.ru",
        # "https://forumd.ru",
        # "https://fourcross.rusff.me",
        # "https://francexvii.rusff.me",
        # "https://freshair.rusff.me",
        # "https://funeralrave.rusff.me",
        # "https://galaxycross.rusff.me",
        # "https://geiger.rusff.me",
        # "https://glassdrop.rusff.me",
        # "https://goldenhour.rusff.me",
        # "https://gosutowarudo.f-rpg.me",
        # "https://grisha.rusff.me",
        # "https://harpy.rusff.me",
        # "https://hollowbones.rusff.me",
        # "https://holod.rusff.me",
        # "https://homecross.f-rpg.ru",
        # "https://homeostasis.rolevaya.com",
        # "https://hotspot.rusff.me",
        # "https://howlongisnow.rusff.me",
        # "https://hpfreakshow.rusff.me",
        # "https://hproleplay.ru",
        # "https://icyou.rusff.me",
        # "https://ignis.rolka.su",
        # "https://illusioncross.rusff.me",
        # "https://imagiart.ru",
        # "https://imaginacion.rusff.me",
        # "https://imperiumaeternum.rolka.su",
        # "https://infinitumcross.rusff.me",
        # "https://insideout.mybb.ru",
        # "https://irepublic.rusff.me",
        # "https://itisanewworld.rusff.me",
        # "https://jkisdead.rusff.me",
        # "https://kingdoms.hutt.ru",
        # "https://kingdomtales.rusff.me",
        # "https://kingscross.f-rpg.ru",
        # "https://kiri.rolka.su",
        # "https://korean-academy.ru",
        # "https://kteonor.mybb.ru",
        # "https://kusabi.mybb.ru",
        # "https://lacommedia.rusff.me",
        # "https://legendsneverdie.hutt.ru",
        # "https://lepidus.ru",
        # "https://levelingup.rusff.me",
        # "https://lib.rusff.me",
        # "https://liberum.f-rpg.me",
        # "https://likeitseoul.rusff.me",
        # "https://longliverock.rusff.me",
        # "https://luminary.f-rpg.ru",
        # "https://mafialand.rolevaya.com",
        # "https://magia.rusff.me",
        # "https://manhattanlife.ru",
        # "https://manifesto.rusff.me",
        # "https://manunkind.rusff.me",
        # "https://maydaykorea.rusff.me",
        # "https://mayhem.rusff.me",
        # "https://mchronicles.rusff.me",
        # "https://measurement.rusff.me",
        # "https://meinspace.rusff.me",
        # "https://memlane.rusff.me",
        # "https://mhshootme.rusff.me",
        # "https://miorline.rolevaya.ru",
        # "https://mirine.rusff.me",
        # "https://misterium-rpg.ru",
        # "https://modao.rolka.su",
        # "https://modaozushi.rolbb.ru",
        # "https://moonshadows.ru",
        # "https://muhtesempire.rusff.me",
        # "https://musicalspace.rusff.me",
        # "https://nevah.ru",
        # "https://neverdie.rusff.me",
        # "https://neversleeps.rusff.me",
        # "https://newyorkbynight.ru",
        # "https://nightsurf.rusff.me",
        # "https://noctum.f-rpg.me",
        # "https://nolf.rusff.me",
        # "https://nomoreutopia.rusff.me",
        # "https://ohcanada.rusff.me",
        # "https://oneway.rusff.me",
        # "https://onlineroleplay.rusff.me",
        # "https://onlyadventure.mybb.ru",
        # "https://openboston.rusff.me",
        # "https://others.rusff.me",
        # "https://ouatuntold.rusff.me",
        # "https://outerspace.rusff.me",
        # "https://padik.rusff.me",
        # "https://paris.f-rpg.ru",
        # "https://pathologic.f-rpg.ru",
        # "https://phoenixlament.f-rpg.ru",
        # "https://pilgrimhotel.rolebb.ru",
        # "https://postfactum.rusff.me",
        # "https://psinacrosstest.rusff.me",
        # "https://rains.rusff.me",
        # "https://ravecross.rusff.me",
        # "https://reilana.mybb.ru",
        # "https://rempetnewstory.rusff.me",
        # "https://repatriates.rusff.me",
        # "https://replay.rusff.me",
        # "https://romanceclub.f-rpg.ru",
        # "https://rpginuyasha.7fi.ru",
        # "https://rpgslayers.7bk.ru",
        # "https://rusmagic.rusff.me",
        # "https://sabbathage.rolka.su",
        # "https://sacramentolife.ru",
        # "https://salaamnamaste.rusff.me",
        # "https://sc.roleforum.ru",
        # "https://sedov.rusff.me",
        # "https://senros.rusff.me",
        # "https://seoulsimulation.rusff.me",
        # "https://sexyfantasy.rolka.su",
        # "https://shardsofpower.rolka.su",
        # "https://shel.rusff.me",
        # "https://shelterme.rusff.me",
        # "https://sherlock.rusff.me",
        # "https://sherwood.rolka.su",
        # "https://sibirsk.rusff.me",
        # "https://sideffect.rusff.me",
        # "https://skt.rolka.me",
        # "https://slowhere.ru",
        # "https://smgg.igraemroli.ru",
        # "https://smpostblue.rusff.me",
        # "https://sochi.rusff.me",
        # "https://somaulte.rusff.me",
        # "https://somethingold.f-rpg.me",
        # "https://songsofnirn.rusff.me",
        # "https://soullove.0pk.ru",
        # "https://stadt.f-rpg.ru",
        # "https://starwarstiu.mybb.ru",
        # "https://stasis.rusff.me",
        # "https://stayalive.rolfor.ru",
        # "https://stigma.rusff.me",
        # "https://strannic.mybb.ru",
        # "https://suafata.f-rpg.me",
        # "https://sueta.rusff.me",
        # "https://summerchronicles.rusff.me",
        # "https://supernaturalhell.f-rpg.me",
        # "https://supportme.rusff.me",
        # "https://swipe.rusff.me",
        # "https://swmedley.rusff.me",
        # "https://swordcoast.rusff.me",
        # "https://symbiosis.rusff.me",
        # "https://the100ac.rusff.me",
        # "https://theancientworld.rusff.me",
        # "https://thecityandthecity.rusff.me",
        # "https://themistfrpg.rusff.me",
        # "https://themostsupernatural.ru",
        # "https://thepilgrims.rusff.me",
        # "https://therapysession.rusff.me",
        # "https://thewalkingdead.f-rpg.ru",
        # "https://thewitcher.f-rpg.ru",
        # "https://timess.rusff.me",
        # "https://timetocross.rusff.me",
        # "https://tmi.f-rpg.ru",
        # "https://toeden.rusff.me",
        # "https://totop.rolka.su",
        # "https://tvddownwardspiral.rusff.me",
        # "https://twelvekingdoms.9bb.ru",
        # "https://urchoice.rolka.su",
        # "https://urhome.rusff.me",
        # "https://uts.rusff.me",
        # "https://versus.rolka.su",
        # "https://victorians.mybb.ru",
        # "https://vipersona.rusff.me",
        # "https://vortex.rusff.me",
        # "https://waldmond.f-rpg.ru",
        # "https://warriorscats.1bb.ru",
        # "https://wbd.mybb.ru",
        # "https://wearethefuture.rusff.me",
        # "https://weirdtales.rusff.me",
        # "https://whatheydo.rusff.me",
        # "https://whitepr.0pk.ru",
        # "https://wickedreign.rusff.me",
        # "https://winxclubnew.mybb.ru",
        # "https://wolves.roleforum.ru",
        # "https://wwft.rusff.me",
        # "https://yantar.rusff.me",
        # "https://domzabveniya.ru",
        # "https://yellowcross.f-rpg.ru",
        # "https://yourbalance.rusff.me",
        # "https://yourphoenix.rusff.me",
        # "https://forcecross.ru",
        # "https://lightsout.f-rpg.me",
        # "https://simpledimple.rusff.me",
        # "https://vraiven.rusff.me",
        # "https://goodtime.rusff.me",
        # "https://razvod.rusff.me",
        # "https://e666yn.f-rpg.me",
        # "https://utoptest.rusff.me",
        # "https://staffage.rusff.me",
        # "https://crisscross.f-rpg.me",
        # "https://shardsofpower.rolka.me",
        # "https://smpostblue.rusff.me",
        # "https://ignis.rolka.me",
        "https://barcross.rusff.me"
    ],
        'https://dis.f-rpg.me',
        'https://dis.f-rpg.me/viewtopic.php?id=502',
        """[align=center][url=https://dis.f-rpg.me/][img]https://forumstatic.ru/files/001a/e7/ed/30940.png[/img][/url]
[url=https://dis.f-rpg.me/viewtopic.php?id=105][b]упрощенный приём[/b][/url] • [url=https://dis.f-rpg.me/viewtopic.php?id=4][b]сюжет[/b][/url] • [url=https://dis.f-rpg.me/viewtopic.php?id=12][b]расы[/b][/url] • [url=https://dis.f-rpg.me/viewtopic.php?id=24][b]гостевая[/b][/url][/align]""",
        '')
    test.select_forum()
